/* Thread reading Oracle Redo Logs
   Copyright (C) 2018-2020 Adam Leszczynski.

This file is part of Open Log Replicator.

Open Log Replicator is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 3, or (at your option)
any later version.

Open Log Replicator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License
along with Open Log Replicator; see the file LICENSE.txt  If not see
<http://www.gnu.org/licenses/>.  */

#include <sys/stat.h>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <unistd.h>
#include <string.h>
#include <rapidjson/document.h>
#include "types.h"

#include "OracleColumn.h"
#include "OracleObject.h"
#include "OracleReader.h"
#include "CommandBuffer.h"
#include "OracleReaderRedo.h"
#include "RedoLogException.h"
#include "OracleStatement.h"
#include "Transaction.h"
#include "TransactionChunk.h"

using namespace std;
using namespace rapidjson;
using namespace oracle::occi;

const Value& getJSONfield(const Document& document, const char* field);

namespace OpenLogReplicator {

    OracleReader::OracleReader(CommandBuffer *commandBuffer, const string alias, const string database, const string user, const string passwd,
            const string connectString, uint64_t trace, uint64_t trace2, uint64_t dumpRedoLog, uint64_t dumpRawData, uint64_t directRead,
            uint64_t redoReadSleep, uint64_t checkpointInterval, uint64_t redoBuffers, uint64_t redoBufferSize, uint64_t maxConcurrentTransactions) :
        Thread(alias, commandBuffer),
        currentRedo(nullptr),
        databaseSequence(0),
        databaseSequenceArchMax(0),
        env(nullptr),
        conn(nullptr),
        user(user),
        passwd(passwd),
        connectString(connectString),
        database(database),
        databaseContext(""),
        databaseScn(0),
        lastOpTransactionMap(maxConcurrentTransactions),
        transactionHeap(maxConcurrentTransactions),
        transactionBuffer(new TransactionBuffer(redoBuffers, redoBufferSize)),
        redoBuffer(new uint8_t[DISK_BUFFER_SIZE * 2]),
        headerBuffer(new uint8_t[REDO_PAGE_SIZE_MAX * 2]),
        recordBuffer(new uint8_t[REDO_RECORD_MAX_SIZE]),
        commandBuffer(commandBuffer),
        dumpRedoLog(dumpRedoLog),
        dumpRawData(dumpRawData),
        directRead(directRead),
        redoReadSleep(redoReadSleep),
        trace(trace),
        trace2(trace2),
        version(0),
        conId(0),
        resetlogs(0),
        previousCheckpoint(clock()),
        checkpointInterval(checkpointInterval),
        bigEndian(false),
        read16(read16Little),
        read32(read32Little),
        read56(read56Little),
        read64(read64Little),
        readSCN(readSCNLittle),
        readSCNr(readSCNrLittle),
        write16(write16Little),
        write32(write32Little),
        write56(write56Little),
        write64(write64Little),
        writeSCN(writeSCNLittle) {

        readCheckpoint();
        env = Environment::createEnvironment (Environment::DEFAULT);
    }

    OracleReader::~OracleReader() {
        while (!archiveRedoQueue.empty()) {
            OracleReaderRedo *redoTmp = archiveRedoQueue.top();
            archiveRedoQueue.pop();
            delete redoTmp;
        }

        if (conn != nullptr) {
            env->terminateConnection(conn);
            conn = nullptr;
        }
        if (env != nullptr) {
            Environment::terminateEnvironment(env);
            env = nullptr;
        }

        delete transactionBuffer;

        for (auto it : objectMap) {
            OracleObject *object = it.second;
            delete object;
        }
        objectMap.clear();

        for (auto it : xidTransactionMap) {
            Transaction *transaction = it.second;
            delete transaction;
        }
        xidTransactionMap.clear();

        if (redoBuffer != nullptr) {
            delete[] redoBuffer;
            redoBuffer = nullptr;
        }

        if (headerBuffer != nullptr) {
            delete[] headerBuffer;
            headerBuffer = nullptr;
        }

        if (recordBuffer != nullptr) {
            delete[] recordBuffer;
            recordBuffer = nullptr;
        }
    }

    void OracleReader::checkConnection(bool reconnect) {
        while (!this->shutdown) {
            if (conn == nullptr) {
                cout << "- connecting to Oracle database " << database << endl;
                try {
                    conn = env->createConnection(user, passwd, connectString);
                } catch(SQLException &ex) {
                    cerr << "ERROR: " << dec << ex.getErrorCode() << ": " << ex.getMessage();
                }
            }

            if (conn != nullptr || !reconnect)
                break;

            cerr << "ERROR: cannot connect to database, retry in 5 sec." << endl;
            sleep(5);
        }
    }

    void *OracleReader::run(void) {
        checkConnection(true);
        cout << "- Oracle Reader for: " << database << endl;
        onlineLogGetList();
        uint64_t ret = REDO_OK;
        OracleReaderRedo *redo = nullptr;
        bool logsProcessed;

        while (true) {
            logsProcessed = false;

            //try to read online redo logs
            if (this->shutdown)
                break;
            if ((trace2 & TRACE2_REDO) != 0)
                cerr << "REDO: checking online redo logs" << endl;
            refreshOnlineLogs();

            while (true) {
                redo = nullptr;
                if ((trace2 & TRACE2_REDO) != 0)
                    cerr << "REDO: searching online redo log for sequence: " << dec << databaseSequence << endl;

                //find the candidate to read
                for (auto redoTmp: onlineRedoSet) {
                    if (redoTmp->sequence == databaseSequence)
                        redo = redoTmp;
                    if ((trace2 & TRACE2_REDO) != 0)
                        cerr << "REDO: " << redoTmp->path << " is " << dec << redoTmp->sequence << endl;
                }

                //keep reading online redo logs while it is possible
                if (redo == nullptr) {
                    bool isHigher = false;
                    while (true) {
                        for (auto redoTmp: onlineRedoSet) {
                            if (redoTmp->sequence > databaseSequence)
                                isHigher = true;
                            if (redoTmp->sequence == databaseSequence)
                                redo = redoTmp;
                        }

                        if (redo == nullptr && !isHigher) {
                            usleep(redoReadSleep);
                        } else
                            break;

                        if (this->shutdown)
                            break;
                        refreshOnlineLogs();
                    }
                }

                if (redo == nullptr)
                    break;

                //if online redo log is overwritten - then switch to reading archive logs
                if (this->shutdown)
                    break;
                logsProcessed = true;
                ret = redo->processLog();

                if (ret != REDO_OK) {
                    if (ret == REDO_WRONG_SEQUENCE_SWITCHED) {
                        if (trace >= TRACE_DETAIL)
                            cerr << "INFO: online redo log overwritten by new data" << endl;
                        break;
                    }
                    cerr << "ERROR: process log returned: " << dec << ret << endl;
                    throw RedoLogException("read archive log", nullptr, 0);
                }

                if (this->shutdown)
                    break;

                ++databaseSequence;
                writeCheckpoint(false);
            }

            //try to read all archived redo logs
            if (this->shutdown)
                break;
            if ((trace2 & TRACE2_REDO) != 0)
                cerr << "REDO: checking archive redo logs" << endl;
            archLogGetList();

            while (!archiveRedoQueue.empty()) {
                OracleReaderRedo *redoPrev = redo;
                redo = archiveRedoQueue.top();
                if ((trace2 & TRACE2_REDO) != 0)
                    cerr << "REDO: searching archived redo log for sequence: " << dec << databaseSequence << endl;

                if (ret == REDO_WRONG_SEQUENCE_SWITCHED && redoPrev != nullptr && redoPrev->sequence == redo->sequence) {
                    if (trace >= TRACE_DETAIL)
                        cerr << "INFO: continuing broken online redo log read process with archive logs" << endl;
                    redo->clone(redoPrev);
                }

                if (redo->sequence < databaseSequence)
                    continue;
                if (redo->sequence > databaseSequence) {
                    cerr << "ERROR: could not find archive log for sequence: " << dec << databaseSequence << ", found: " << redo->sequence << " instead" << endl;
                    throw RedoLogException("read archive log", nullptr, 0);
                }

                if (this->shutdown)
                    break;
                logsProcessed = true;
                ret = redo->processLog();

                if (ret != REDO_OK) {
                    cerr << "ERROR: archive log processing returned: " << dec << ret << endl;
                    throw RedoLogException("read archive log", nullptr, 0);
                }

                ++databaseSequence;
                writeCheckpoint(false);
                archiveRedoQueue.pop();
                delete redo;
                redo = nullptr;
            }

            if (this->shutdown)
                break;
            if (!logsProcessed)
                usleep(redoReadSleep);
        }

        writeCheckpoint(true);
        dumpTransactions();

        return 0;
    }

    void OracleReader::archLogGetList() {
        checkConnection(true);

        try {
            OracleStatement stmt(&conn, env);
            stmt.createStatement("SELECT NAME, SEQUENCE#, FIRST_CHANGE#, FIRST_TIME, NEXT_CHANGE#, NEXT_TIME FROM SYS.V_$ARCHIVED_LOG WHERE SEQUENCE# >= :i AND RESETLOGS_ID = :i AND NAME IS NOT NULL ORDER BY SEQUENCE#, DEST_ID");
            stmt.stmt->setInt(1, databaseSequence);
            stmt.stmt->setInt(2, resetlogs);
            stmt.executeQuery();

            string path;
            typeseq sequence;
            typescn firstScn, nextScn;

            while (stmt.rset->next()) {
                path = stmt.rset->getString(1);
                sequence = stmt.rset->getNumber(2);
                firstScn = stmt.rset->getNumber(3);
                nextScn = stmt.rset->getNumber(5);

                OracleReaderRedo* redo = new OracleReaderRedo(this, 0, path.c_str());
                redo->firstScn = firstScn;
                redo->nextScn = nextScn;
                redo->sequence = sequence;
                archiveRedoQueue.push(redo);
            }
        } catch(SQLException &ex) {
            cerr << "ERROR: getting arch log list: " << dec << ex.getErrorCode() << ": " << ex.getMessage();
        }
    }

    void OracleReader::onlineLogGetList() {
        checkConnection(true);

        int64_t groupLast = -1, group = -1, groupPrev = -1;
        struct stat fileStat;
        string path;

        try {
            OracleStatement stmt(&conn, env);
            stmt.createStatement("SELECT LF.GROUP#, LF.MEMBER FROM SYS.V_$LOGFILE LF ORDER BY LF.GROUP# ASC, LF.IS_RECOVERY_DEST_FILE DESC, LF.MEMBER ASC");
            stmt.executeQuery();

            while (stmt.rset->next()) {
                groupPrev = group;
                group = stmt.rset->getNumber(1);
                path = stmt.rset->getString(2);

                if (groupPrev != groupLast && group != groupPrev) {
                    throw RedoLogException("can't read any member from group", nullptr, 0);
                }

                if (group != groupLast && stat(path.c_str(), &fileStat) == 0) {
                    cerr << "Found log GROUP: " << group << " PATH: " << path << endl;
                    OracleReaderRedo* redo = new OracleReaderRedo(this, group, path.c_str());
                    onlineRedoSet.insert(redo);
                    groupLast = group;
                }
            }
        } catch(SQLException &ex) {
            throw RedoLogException("errog getting online log list", nullptr, 0);
        }

        if (group != groupLast) {
            throw RedoLogException("can't read any member from group", nullptr, 0);
        }
    }

    void OracleReader::refreshOnlineLogs() {
        for (auto redoTmp: onlineRedoSet) {
            redoTmp->reload();
        }
    }

    OracleObject *OracleReader::checkDict(typeobj objn, typeobj objd) {
        OracleObject *object = objectMap[objn];
        return object;
    }

    void OracleReader::addToDict(OracleObject *object) {
        if (objectMap[object->objn] == nullptr) {
            objectMap[object->objn] = object;
        }
    }

    uint16_t OracleReader::read16Little(const uint8_t* buf) {
        return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }

    uint16_t OracleReader::read16Big(const uint8_t* buf) {
        return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    }

    uint32_t OracleReader::read32Little(const uint8_t* buf) {
        return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }

    uint32_t OracleReader::read32Big(const uint8_t* buf) {
        return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    }

    uint64_t OracleReader::read56Little(const uint8_t* buf) {
        return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
                ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
                ((uint64_t)buf[6] << 48);
    }

    uint64_t OracleReader::read56Big(const uint8_t* buf) {
        return (((uint64_t)buf[0] << 48) | ((uint64_t)buf[1] << 40) |
                ((uint64_t)buf[2] << 32) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[4] << 16) | ((uint64_t)buf[5] << 8) |
                (uint64_t)buf[6]);
    }

    uint64_t OracleReader::read64Little(const uint8_t* buf) {
        return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
                ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
                ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
    }

    uint64_t OracleReader::read64Big(const uint8_t* buf) {
        return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
                ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
                ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
                ((uint64_t)buf[6] << 8) | (uint64_t)buf[7];
    }

    typescn OracleReader::readSCNLittle(const uint8_t* buf) {
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF && buf[5] == 0xFF)
            return ZERO_SCN;
        if ((buf[5] & 0x80) == 0x80)
            return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
                ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[6] << 32) | ((uint64_t)buf[7] << 40) |
                ((uint64_t)buf[4] << 48) | ((uint64_t)(buf[5] & 0x7F) << 56);
        else
            return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
                ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
                ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40);
    }

    typescn OracleReader::readSCNBig(const uint8_t* buf) {
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF && buf[5] == 0xFF)
            return ZERO_SCN;
        if ((buf[0] & 0x80) == 0x80)
            return (uint64_t)buf[5] | ((uint64_t)buf[4] << 8) |
                ((uint64_t)buf[3] << 16) | ((uint64_t)buf[2] << 24) |
                ((uint64_t)buf[7] << 32) | ((uint64_t)buf[6] << 40) |
                ((uint64_t)buf[1] << 48) | ((uint64_t)(buf[0] & 0x7F) << 56);
        else
            return (uint64_t)buf[5] | ((uint64_t)buf[4] << 8) |
                ((uint64_t)buf[3] << 16) | ((uint64_t)buf[2] << 24) |
                ((uint64_t)buf[1] << 32) | ((uint64_t)buf[0] << 40);
    }

    typescn OracleReader::readSCNrLittle(const uint8_t* buf) {
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF && buf[5] == 0xFF)
            return ZERO_SCN;
        if ((buf[1] & 0x80) == 0x80)
            return (uint64_t)buf[2] | ((uint64_t)buf[3] << 8) |
                ((uint64_t)buf[4] << 16) | ((uint64_t)buf[5] << 24) |
                //((uint64_t)buf[6] << 32) | ((uint64_t)buf[7] << 40) |
                ((uint64_t)buf[0] << 48) | ((uint64_t)(buf[1] & 0x7F) << 56);
        else
            return (uint64_t)buf[2] | ((uint64_t)buf[3] << 8) |
                ((uint64_t)buf[4] << 16) | ((uint64_t)buf[5] << 24) |
                ((uint64_t)buf[0] << 32) | ((uint64_t)buf[1] << 40);
    }

    typescn OracleReader::readSCNrBig(const uint8_t* buf) {
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF && buf[5] == 0xFF)
            return ZERO_SCN;
        if ((buf[1] & 0x80) == 0x80)
            return (uint64_t)buf[5] | ((uint64_t)buf[4] << 8) |
                ((uint64_t)buf[3] << 16) | ((uint64_t)buf[2] << 24) |
                //((uint64_t)buf[7] << 32) | ((uint64_t)buf[6] << 40) |
                ((uint64_t)buf[1] << 48) | ((uint64_t)(buf[0] & 0x7F) << 56);
        else
            return (uint64_t)buf[5] | ((uint64_t)buf[4] << 8) |
                ((uint64_t)buf[3] << 16) | ((uint64_t)buf[2] << 24) |
                ((uint64_t)buf[1] << 32) | ((uint64_t)buf[0] << 40);
    }

    void OracleReader::write16Little(uint8_t* buf, uint16_t val) {
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
    }

    void OracleReader::write16Big(uint8_t* buf, uint16_t val) {
        buf[0] = (val >> 8) & 0xFF;
        buf[1] = val & 0xFF;
    }

    void OracleReader::write32Little(uint8_t* buf, uint32_t val) {
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
        buf[2] = (val >> 16) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
    }

    void OracleReader::write32Big(uint8_t* buf, uint32_t val) {
        buf[0] = (val >> 24) & 0xFF;
        buf[1] = (val >> 16) & 0xFF;
        buf[2] = (val >> 8) & 0xFF;
        buf[3] = val & 0xFF;
    }

    void OracleReader::write56Little(uint8_t* buf, uint64_t val) {
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
        buf[2] = (val >> 16) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
        buf[4] = (val >> 32) & 0xFF;
        buf[5] = (val >> 40) & 0xFF;
        buf[6] = (val >> 48) & 0xFF;
    }

    void OracleReader::write56Big(uint8_t* buf, uint64_t val) {
        buf[0] = (val >> 48) & 0xFF;
        buf[1] = (val >> 40) & 0xFF;
        buf[2] = (val >> 32) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
        buf[4] = (val >> 16) & 0xFF;
        buf[5] = (val >> 8) & 0xFF;
        buf[6] = val & 0xFF;
    }

    void OracleReader::write64Little(uint8_t* buf, uint64_t val) {
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
        buf[2] = (val >> 16) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
        buf[4] = (val >> 32) & 0xFF;
        buf[5] = (val >> 40) & 0xFF;
        buf[6] = (val >> 48) & 0xFF;
        buf[7] = (val >> 56) & 0xFF;
    }

    void OracleReader::write64Big(uint8_t* buf, uint64_t val) {
        buf[0] = (val >> 56) & 0xFF;
        buf[1] = (val >> 48) & 0xFF;
        buf[2] = (val >> 40) & 0xFF;
        buf[3] = (val >> 32) & 0xFF;
        buf[4] = (val >> 24) & 0xFF;
        buf[5] = (val >> 16) & 0xFF;
        buf[6] = (val >> 8) & 0xFF;
        buf[7] = val & 0xFF;
    }

    void OracleReader::writeSCNLittle(uint8_t* buf, typescn val) {
        if (val < 0x800000000000) {
            buf[0] = val & 0xFF;
            buf[1] = (val >> 8) & 0xFF;
            buf[2] = (val >> 16) & 0xFF;
            buf[3] = (val >> 24) & 0xFF;
            buf[4] = (val >> 32) & 0xFF;
            buf[5] = (val >> 40) & 0xFF;
        } else {
            buf[0] = val & 0xFF;
            buf[1] = (val >> 8) & 0xFF;
            buf[2] = (val >> 16) & 0xFF;
            buf[3] = (val >> 24) & 0xFF;
            buf[4] = (val >> 48) & 0xFF;
            buf[5] = ((val >> 56) & 0xFF) | 0x80;
            buf[6] = (val >> 32) & 0xFF;
            buf[7] = (val >> 40) & 0xFF;
        }
    }

    void OracleReader::writeSCNBig(uint8_t* buf, typescn val) {
        if (val < 0x800000000000) {
            buf[5] = val & 0xFF;
            buf[4] = (val >> 8) & 0xFF;
            buf[3] = (val >> 16) & 0xFF;
            buf[2] = (val >> 24) & 0xFF;
            buf[1] = (val >> 32) & 0xFF;
            buf[0] = (val >> 40) & 0xFF;
        } else {
            buf[5] = val & 0xFF;
            buf[4] = (val >> 8) & 0xFF;
            buf[3] = (val >> 16) & 0xFF;
            buf[2] = (val >> 24) & 0xFF;
            buf[1] = (val >> 48) & 0xFF;
            buf[0] = ((val >> 56) & 0xFF) | 0x80;
            buf[7] = (val >> 32) & 0xFF;
            buf[6] = (val >> 40) & 0xFF;
        }
    }


    uint64_t OracleReader::initialize() {
        checkConnection(false);
        if (conn == nullptr)
            return 0;

        typescn currentDatabaseScn;
        typeresetlogs currentResetlogs;

        try {
            OracleStatement stmt(&conn, env);
            //check archivelog mode, supplemental log min, endian
            stmt.createStatement("SELECT D.LOG_MODE, D.SUPPLEMENTAL_LOG_DATA_MIN, TP.ENDIAN_FORMAT, D.CURRENT_SCN, DI.RESETLOGS_ID, VER.BANNER, SYS_CONTEXT('USERENV','DB_NAME') AS DB_NAME FROM SYS.V_$DATABASE D JOIN SYS.V_$TRANSPORTABLE_PLATFORM TP ON TP.PLATFORM_NAME = D.PLATFORM_NAME JOIN SYS.V_$VERSION VER ON VER.BANNER LIKE '%Oracle Database%' JOIN SYS.V_$DATABASE_INCARNATION DI ON DI.STATUS = 'CURRENT'");
            stmt.executeQuery();

            if (stmt.rset->next()) {
                string LOG_MODE = stmt.rset->getString(1);
                if (LOG_MODE.compare("ARCHIVELOG") != 0) {
                    cerr << "ERROR: database not in ARCHIVELOG mode. RUN: " << endl;
                    cerr << " SHUTDOWN IMMEDIATE;" << endl;
                    cerr << " STARTUP MOUNT;" << endl;
                    cerr << " ALTER DATABASE ARCHIVELOG;" << endl;
                    cerr << " ALTER DATABASE OPEN;" << endl;
                    return 0;
                }

                string SUPPLEMENTAL_LOG_MIN = stmt.rset->getString(2);
                if (SUPPLEMENTAL_LOG_MIN.compare("YES") != 0) {
                    cerr << "Error: SUPPLEMENTAL_LOG_DATA_MIN missing: RUN:" << endl;
                    cerr << " ALTER DATABASE ADD SUPPLEMENTAL LOG DATA;" << endl;
                    cerr << " ALTER SYSTEM ARCHIVE LOG CURRENT;" << endl;
                    return 0;
                }

                string ENDIANNESS = stmt.rset->getString(3);
                if (ENDIANNESS.compare("Big") == 0) {
                    bigEndian = true;
                    read16 = read16Big;
                    read32 = read32Big;
                    read56 = read56Big;
                    read64 = read64Big;
                    readSCN = readSCNBig;
                    readSCNr = readSCNrBig;
                    write16 = write16Big;
                    write32 = write32Big;
                    write56 = write56Big;
                    write64 = write64Big;
                    writeSCN = writeSCNBig;
                }

                currentDatabaseScn = stmt.rset->getNumber(4);
                currentResetlogs = stmt.rset->getNumber(5);
                if (resetlogs != 0 && currentResetlogs != resetlogs) {
                    cerr << "Error: Incorrect database incarnation. Previous resetlogs:" << dec << resetlogs << ", current: " << currentResetlogs << endl;
                    return 0;
                } else {
                    resetlogs = currentResetlogs;
                }

                //12+
                string VERSION = stmt.rset->getString(6);
                cout << "- version: " << dec << VERSION << endl;

                conId = 0;
                if (VERSION.find("Oracle Database 11g") == string::npos) {
                    OracleStatement stmt(&conn, env);
                    stmt.createStatement("select sys_context('USERENV','CON_ID') CON_ID from DUAL");
                    stmt.executeQuery();

                    if (stmt.rset->next()) {
                        conId = stmt.rset->getNumber(1);
                        cout << "- conId: " << dec << conId << endl;
                    }
                }

                databaseContext = stmt.rset->getString(7);

            } else {
                cerr << "ERROR: reading SYS.V_$DATABASE" << endl;
                return 0;
            }
        } catch(SQLException &ex) {
            cerr << "ERROR: " << dec << ex.getErrorCode() << ": " << ex.getMessage();
            return 0;
        }

        if (databaseSequence == 0 || databaseScn == 0) {
            try {
                OracleStatement stmt(&conn, env);
                stmt.createStatement("select SEQUENCE# from SYS.V_$LOG where status = 'CURRENT'");
                stmt.executeQuery();

                if (stmt.rset->next()) {
                    databaseSequence = stmt.rset->getNumber(1);
                    databaseScn = currentDatabaseScn;
                }
            } catch(SQLException &ex) {
                cerr << "ERROR: " << dec << ex.getErrorCode() << ": " << ex.getMessage();
            }
        }

        cout << "- sequence: " << dec << databaseSequence << endl;
        cout << "- scn: " << dec << databaseScn << endl;
        cout << "- resetlogs: " << dec << resetlogs << endl;

        if (databaseSequence == 0 || databaseScn == 0)
            return 0;
        else
            return 1;
    }

    void OracleReader::addTable(string mask, uint64_t options) {
        checkConnection(false);
        cout << "- reading table schema for: " << mask;
        uint64_t tabCnt = 0;

        try {
            OracleStatement stmt(&conn, env);
            OracleStatement stmt2(&conn, env);
            stmt.createStatement(
                    "SELECT tab.DATAOBJ# as objd, tab.OBJ# as objn, tab.CLUCOLS as clucols, usr.USERNAME AS owner, obj.NAME AS objectName, decode(bitand(tab.FLAGS, 8388608), 8388608, 1, 0) as dependencies "
                    "FROM SYS.TAB$ tab, SYS.OBJ$ obj, ALL_USERS usr "
                    "WHERE tab.OBJ# = obj.OBJ# "
                    "AND obj.OWNER# = usr.USER_ID "
                    "AND usr.USERNAME || '.' || obj.NAME LIKE :i");
            stmt.stmt->setString(1, mask);

            stmt.executeQuery();
            while (stmt.rset->next()) {
                //skip partitioned/IOT tables
                string owner = stmt.rset->getString(4);
                string objectName = stmt.rset->getString(5);
                typeobj objn = stmt.rset->getNumber(2);
                if (stmt.rset->isNull(1)) {
                    cout << endl << "  * skipped: " << owner << "." << objectName << " (OBJN: " << dec << objn << ") - partitioned or IOT";
                } else {
                    typeobj objd = stmt.rset->getNumber(1);
                    uint64_t cluCols = 0;
                    if (!stmt.rset->isNull(3))
                        stmt.rset->getNumber(3);
                    uint64_t depdendencies = stmt.rset->getNumber(6);
                    uint64_t totalPk = 0, totalCols = 0;
                    OracleObject *object = new OracleObject(objn, objd, depdendencies, cluCols, options, owner, objectName);
                    ++tabCnt;

                    cout << endl << "  * found: " << owner << "." << objectName << " (OBJD: " << dec << objd << ", OBJN: " << dec << objn << ", DEP: " << dec << depdendencies << ")";

                    stmt2.createStatement("SELECT C.COL#, C.SEGCOL#, C.NAME, C.TYPE#, C.LENGTH, C.PRECISION#, C.SCALE, C.NULL$, (SELECT COUNT(*) FROM SYS.CCOL$ L JOIN SYS.CDEF$ D on D.con# = L.con# AND D.type# = 2 WHERE L.intcol# = C.intcol# and L.obj# = C.obj#) AS NUMPK FROM SYS.COL$ C WHERE C.OBJ# = :i ORDER BY C.SEGCOL#");
                    stmt2.stmt->setInt(1, objn);
                    stmt2.executeQuery();

                    while (stmt2.rset->next()) {
                        uint64_t colNo = stmt2.rset->getNumber(1);
                        uint64_t segColNo = stmt2.rset->getNumber(2);
                        string columnName = stmt2.rset->getString(3);
                        uint64_t typeNo = stmt2.rset->getNumber(4);
                        uint64_t length = stmt2.rset->getNumber(5);
                        int64_t precision = -1;
                        if (!stmt2.rset->isNull(6))
                            precision = stmt2.rset->getNumber(6);
                        int64_t scale = -1;
                        if (!stmt2.rset->isNull(7))
                            scale = stmt2.rset->getNumber(7);

                        int64_t nullable = stmt2.rset->getNumber(8);
                        uint64_t numPk = stmt2.rset->getNumber(9);
                        OracleColumn *column = new OracleColumn(colNo, segColNo, columnName, typeNo, length, precision, scale, numPk, (nullable == 0));
                        totalPk += numPk;
                        ++totalCols;

                        object->addColumn(column);
                    }

                    object->totalCols = totalCols;
                    object->totalPk = totalPk;
                    addToDict(object);
                }
            }
        } catch(SQLException &ex) {
            cerr << "ERROR: getting table metadata: " << dec << ex.getErrorCode() << ": " << ex.getMessage();
        }
        cout << " (total: " << dec << tabCnt << ")" << endl;
    }

    void OracleReader::readCheckpoint() {
        ifstream infile;
        infile.open((database + ".json").c_str(), ios::in);
        if (!infile.is_open())
            return;

        string configJSON((istreambuf_iterator<char>(infile)), istreambuf_iterator<char>());
        Document document;

        if (configJSON.length() == 0 || document.Parse(configJSON.c_str()).HasParseError())
            {cerr << "ERROR: parsing " << database << ".json at byte " << dec << document.GetErrorOffset() << endl; infile.close(); return; }

        const Value& databaseJSON = getJSONfield(document, "database");
        if (database.compare(databaseJSON.GetString()) != 0)
            {cerr << "ERROR: bad JSON, invalid database name (" << databaseJSON.GetString() << ")!" << endl; infile.close(); return; }

        const Value& databaseSequenceJSON = getJSONfield(document, "sequence");
        databaseSequence = databaseSequenceJSON.GetUint64();

        const Value& resetlogsJSON = getJSONfield(document, "resetlogs");
        resetlogs = resetlogsJSON.GetUint64();

        const Value& scnJSON = getJSONfield(document, "scn");
        databaseScn = scnJSON.GetUint64();

        infile.close();
    }

    void OracleReader::writeCheckpoint(bool atShutdown) {
        clock_t now = clock();
        typeseq minSequence = 0xFFFFFFFF;
        Transaction *transaction;

        for (uint64_t i = 1; i <= transactionHeap.heapSize; ++i) {
            transaction = transactionHeap.heap[i];
            if (minSequence > transaction->firstSequence)
                minSequence = transaction->firstSequence;
        }
        if (minSequence == 0xFFFFFFFF)
            minSequence = databaseSequence;

        if (trace >= TRACE_FULL) {
            uint64_t timeSinceCheckpoint = (now - previousCheckpoint) / CLOCKS_PER_SEC;

            if (version >= 0x12200)
                cerr << "INFO: Writing checkpoint information SEQ: " << dec << minSequence << "/" << databaseSequence <<
                " SCN: " << PRINTSCN64(databaseScn) << " after: " << dec << timeSinceCheckpoint << "s" << endl;
            else
                cerr << "INFO: Writing checkpoint information SEQ: " << dec << minSequence << "/" << databaseSequence <<
                " SCN: " << PRINTSCN48(databaseScn) << " after: " << dec << timeSinceCheckpoint << "s" << endl;
        }

        ofstream outfile;
        outfile.open((database + ".json").c_str(), ios::out | ios::trunc);

        if (!outfile.is_open()) {
            cerr << "ERROR: writing checkpoint data for " << database << endl;
            return;
        }

        stringstream ss;
        ss << "{" << endl
           << "  \"database\": \"" << database << "\"," << endl
           << "  \"sequence\": " << dec << minSequence << "," << endl
           << "  \"scn\": " << dec << databaseScn << "," << endl
           << "  \"resetlogs\": " << dec << resetlogs << endl
           << "}";

        outfile << ss.rdbuf();
        outfile.close();

        if (atShutdown) {
            cerr << "Writing checkpopint at exit for " << database << endl
                    << "- conId: " << dec << conId << endl
                    << "- sequence: " << dec << minSequence << endl
                    << "- scn: " << dec << databaseScn << endl
                    << "- resetlogs: " << dec << resetlogs << endl;
        }

        previousCheckpoint = now;
    }

    void OracleReader::checkForCheckpoint() {
        uint64_t timeSinceCheckpoint = (clock() - previousCheckpoint) / CLOCKS_PER_SEC;
        if (timeSinceCheckpoint > checkpointInterval) {
            if (trace >= TRACE_FULL) {
                cerr << "INFO: Time since last checkpoint: " << dec << timeSinceCheckpoint << "s, forcing checkpoint" << endl;
            }
            writeCheckpoint(false);
        } else {
            if (trace >= TRACE_FULL) {
                cerr << "INFO: Time since last checkpoint: " << dec << timeSinceCheckpoint << "s" << endl;
            }
        }
    }

    void OracleReader::dumpTransactions() {
        if (trace >= TRACE_INFO) {
            cerr << "INFO: free buffers: " << dec << transactionBuffer->freeBuffers << "/" << transactionBuffer->redoBuffers << endl;
            if (transactionHeap.heapSize > 0)
                cerr << "INFO: Transactions open: " << dec << transactionHeap.heapSize << endl;
            for (uint64_t i = 1; i <= transactionHeap.heapSize; ++i)
                cerr << "INFO: transaction[" << i << "]: " << *transactionHeap.heap[i] << endl;
        }
    }

    bool OracleReaderRedoCompare::operator()(OracleReaderRedo* const& p1, OracleReaderRedo* const& p2) {
        return p1->sequence > p2->sequence;
    }

    bool OracleReaderRedoCompareReverse::operator()(OracleReaderRedo* const& p1, OracleReaderRedo* const& p2) {
        return p1->sequence < p2->sequence;
    }
}
