// Copyright 2014 GSI, Inc. All rights reserved.
//
//
//

// DDS
#include "CommanderChannel.h"
#include "EnvProp.h"
#include "UserDefaults.h"
// BOOST
#include <boost/filesystem.hpp>

using namespace MiscCommon;
using namespace dds;
using namespace dds::protocol_api;
using namespace dds::agent_cmd;
using namespace std;
using namespace user_defaults_api;
using namespace topology_api;
namespace fs = boost::filesystem;

const uint16_t g_MaxConnectionAttempts = 5;

CCommanderChannel::CCommanderChannel(boost::asio::io_context& _service,
                                     uint64_t _ProtocolHeaderID,
                                     boost::asio::io_context& _intercomService)
    : CClientChannelImpl<CCommanderChannel>(_service, EChannelType::AGENT, _ProtocolHeaderID)
{
    // Create shared memory channel for message forwarding from the network channel
    const CUserDefaults& userDefaults = CUserDefaults::instance();

    m_intercomChannel = CSMIntercomChannel::makeNew(_intercomService,
                                                    userDefaults.getSMLeaderInputNames(),
                                                    userDefaults.getSMLeaderOutputName(_ProtocolHeaderID),
                                                    _ProtocolHeaderID,
                                                    EMQOpenType::OpenOrCreate,
                                                    EMQOpenType::OpenOrCreate);

    m_intercomChannel->registerHandler<cmdCUSTOM_CMD>(
        [this](const SSenderInfo& _sender, SCommandAttachmentImpl<cmdCUSTOM_CMD>::ptr_t _attachment) {
            pushMsg<cmdCUSTOM_CMD>(*_attachment, _sender.m_ID);
        });

    m_intercomChannel->registerHandler<cmdUPDATE_KEY>(
        [this](const SSenderInfo& _sender, SCommandAttachmentImpl<cmdUPDATE_KEY>::ptr_t _attachment) {
            send_cmdUPDATE_KEY(_sender, _attachment);
        });

    m_intercomChannel->start();

    registerHandler<EChannelEvents::OnRemoteEndDissconnected>([this](const SSenderInfo& _sender) {
        if (m_connectionAttempts <= g_MaxConnectionAttempts)
        {
            LOG(info) << "Commander server has dropped the connection. Trying to reconnect. Attempt "
                      << m_connectionAttempts << " out of " << g_MaxConnectionAttempts;
            this_thread::sleep_for(chrono::seconds(5));
            reconnect();
            ++m_connectionAttempts;
        }
        else
        {
            LOG(info) << "Commander server has disconnected. Sending yourself a shutdown command.";
            this->sendYourself<cmdSHUTDOWN>();
        }
    });

    registerHandler<EChannelEvents::OnFailedToConnect>([this](const SSenderInfo& _sender) {
        if (m_connectionAttempts <= g_MaxConnectionAttempts)
        {
            LOG(info) << "Failed to connect to commander server. Trying to reconnect. Attempt " << m_connectionAttempts
                      << " out of " << g_MaxConnectionAttempts;
            this_thread::sleep_for(chrono::seconds(5));
            reconnect();
            ++m_connectionAttempts;
        }
        else
        {
            LOG(info) << "Failed to connect to commander server. Sending yourself a shutdown command.";
            this->sendYourself<cmdSHUTDOWN>();
        }
    });
}

void CCommanderChannel::setNumberOfSlots(size_t _nSlots)
{
    m_nSlots = _nSlots;
}

bool CCommanderChannel::on_cmdREPLY(protocol_api::SCommandAttachmentImpl<protocol_api::cmdREPLY>::ptr_t _attachment,
                                    protocol_api::SSenderInfo& _sender)
{
    switch (_attachment->m_srcCommand)
    {
        case cmdLOBBY_MEMBER_INFO:
        {
            if (_attachment->m_statusCode == (uint16_t)SReplyCmd::EStatusCode::OK)
            {
                LOG(info) << "Received confirmation from lobby leader";
                LOG(info) << "Sending handshake to commander with PHID: " << this->m_protocolHeaderID;

                // Prepare a hand shake message
                SVersionCmd cmd;
                cmd.m_channelType = EChannelType::AGENT;
                cmd.m_sSID = CUserDefaults::instance().getLockedSID();
                cmd.m_version = DDS_PROTOCOL_VERSION;
                pushMsg<cmdLOBBY_MEMBER_HANDSHAKE>(cmd, this->m_protocolHeaderID);

                return true;
            }
            else if (_attachment->m_statusCode == (uint16_t)SReplyCmd::EStatusCode::ERROR)
            {
                LOG(error) << "Received error from lobby leader: " << _attachment->m_sMsg;
                return true;
            }
        }
        break;

        case cmdLOBBY_MEMBER_HANDSHAKE:
        {
            if (_attachment->m_statusCode == (uint16_t)SReplyCmd::EStatusCode::OK)
            {
                LOG(info) << "SM: Handshake successfull. PHID: " << this->m_protocolHeaderID;
                return true;
            }
            else if (_attachment->m_statusCode == (uint16_t)SReplyCmd::EStatusCode::ERROR)
            {
                LOG(fatal) << "SM: Handshake failed. PHID: " << this->m_protocolHeaderID;
                return false;
            }
        }
        break;

        default:
            break;
    }

    return true;
}

bool CCommanderChannel::on_cmdSIMPLE_MSG(SCommandAttachmentImpl<cmdSIMPLE_MSG>::ptr_t _attachment, SSenderInfo& _sender)
{
    switch (_attachment->m_srcCommand)
    {
        case cmdTRANSPORT_TEST:
        {
            pushMsg<cmdSIMPLE_MSG>(*_attachment);
            return true;
        }
        case cmdUPDATE_KEY:
        case cmdCUSTOM_CMD:
        {
            if (_attachment->m_msgSeverity == MiscCommon::error)
            {
                LOG(MiscCommon::error) << _attachment->m_sMsg;
            }
            // Forward message to user task
            m_intercomChannel->pushMsg<cmdSIMPLE_MSG>(*_attachment, _sender.m_ID, _sender.m_ID);
            return true;
        }

        default:
            LOG(debug) << "Received command cmdSIMPLE_MSG does not have a listener";
            return true;
    }

    return true;
}

bool CCommanderChannel::on_cmdGET_HOST_INFO(SCommandAttachmentImpl<cmdGET_HOST_INFO>::ptr_t _attachment,
                                            SSenderInfo& _sender)
{
    // pid
    pid_t pid = getpid();

    SHostInfoCmd cmd;
    get_cuser_name(&cmd.m_username);
    get_hostname(&cmd.m_host);
    cmd.m_version = PROJECT_VERSION_STRING;
    cmd.m_DDSPath = CUserDefaults::getDDSPath();
    cmd.m_agentPid = pid;
    cmd.m_slots = m_nSlots;

    // get worker ID
    string sWorkerId;
    char* pchWorkerId;
    pchWorkerId = getenv("DDS_WORKER_ID");
    if (NULL != pchWorkerId)
    {
        sWorkerId.assign(pchWorkerId);
        cmd.m_workerId = sWorkerId;
    }

    // get submit time
    string sSubmitTime;
    char* pchSubmitTime;
    pchSubmitTime = getenv("DDS_WN_SUBMIT_TIMESTAMP");
    if (NULL != pchSubmitTime)
    {
        sSubmitTime.assign(pchSubmitTime);
        cmd.m_submitTime = stoll(sSubmitTime);
    }

    pushMsg<cmdREPLY_HOST_INFO>(cmd);
    return true;
}

bool CCommanderChannel::on_cmdSHUTDOWN(SCommandAttachmentImpl<cmdSHUTDOWN>::ptr_t _attachment, SSenderInfo& _sender)
{
    deleteAgentIDFile();
    LOG(info) << "The Agent [" << m_id << "] received cmdSHUTDOWN.";
    // return false to let connection manager to catch this message as well
    return false;
}

bool CCommanderChannel::on_cmdBINARY_ATTACHMENT_RECEIVED(
    SCommandAttachmentImpl<cmdBINARY_ATTACHMENT_RECEIVED>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(debug) << "Received command cmdBINARY_ATTACHMENT_RECEIVED";

    switch (_attachment->m_srcCommand)
    {
        case cmdTRANSPORT_TEST:
        {
            // Remove received file
            boost::filesystem::remove(_attachment->m_receivedFilePath);

            pushMsg<cmdBINARY_ATTACHMENT_RECEIVED>(*_attachment);
            return true;
        }
        case cmdASSIGN_USER_TASK:
        {
            boost::filesystem::path destFilePath(CUserDefaults::instance().getSlotsRootDir());
            destFilePath /= to_string(_sender.m_ID);
            destFilePath /= _attachment->m_requestedFileName;
            boost::filesystem::rename(_attachment->m_receivedFilePath, destFilePath);
            // Add exec permissions for the users' task
            fs::permissions(destFilePath, fs::add_perms | fs::owner_exe);
            LOG(info) << "Received user executable to execute: " << destFilePath.generic_string();

            // Send response back to server
            pushMsg<cmdREPLY>(SReplyCmd("File received", (uint16_t)SReplyCmd::EStatusCode::OK, 0, cmdASSIGN_USER_TASK),
                              _sender.m_ID);
            return true;
        }
        case cmdUPDATE_TOPOLOGY:
        {
            // Copy topology file
            boost::filesystem::path destFilePath(CUserDefaults::instance().getDDSPath());
            destFilePath /= _attachment->m_requestedFileName;
            boost::filesystem::rename(_attachment->m_receivedFilePath, destFilePath);
            LOG(info) << "Received new topology file: " << destFilePath.generic_string();

            // Activating new topology
            CTopoCore topo;
            // Topology already validated on the commander, no need to validate it again
            topo.setXMLValidationDisabled(true);
            topo.init(destFilePath.string());
            // Assign new topology
            m_topo = topo;
            LOG(info) << "Topology activated";

            // Send response back to server
            pushMsg<cmdREPLY>(SReplyCmd("File received", (uint16_t)SReplyCmd::EStatusCode::OK, 0, cmdUPDATE_TOPOLOGY));

            return true;
        }
        default:
            LOG(debug) << "Received command cmdBINARY_ATTACHMENT_RECEIVED does not have a listener";
            return true;
    }

    return true;
}

bool CCommanderChannel::on_cmdGET_ID(SCommandAttachmentImpl<cmdGET_ID>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(info) << "cmdGET_ID received from DDS Server";

    SIDCmd msg_cmd;
    msg_cmd.m_id = m_id;
    pushMsg<cmdREPLY_ID>(msg_cmd);

    return true;
}

bool CCommanderChannel::on_cmdSET_ID(SCommandAttachmentImpl<cmdSET_ID>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(info) << "cmdSET_ID attachment [" << *_attachment << "] from DDS Server";

    m_id = _attachment->m_id;

    createAgentIDFile();

    return true;
}

bool CCommanderChannel::on_cmdGET_LOG(SCommandAttachmentImpl<cmdGET_LOG>::ptr_t _attachment, SSenderInfo& _sender)
{
    try
    {
        string hostname;
        get_hostname(&hostname);

        time_t now = chrono::system_clock::to_time_t(chrono::system_clock::now());
        struct tm* ptm = localtime(&now);
        char buffer[20];
        strftime(buffer, 20, "%Y-%m-%d-%H-%M-%S", ptm);

        stringstream ss;
        // TODO: change the code below once on gcc5+
        // We do not use put_time for the moment as gcc4.9 does not support it.
        // ss << std::put_time(ptm, "%Y-%m-%d-%H-%M-%S") << "_" << hostname << "_" << m_id;
        ss << buffer << "_" << hostname << "_" << m_id << ".tar.gz";

        fs::path fileName(ss.str());
        fs::path logDir(CUserDefaults::getDDSPath());
        fs::path filePath(logDir);
        filePath /= fileName;

        // Find command paths in $PATH
        fs::path bashPath = bp::search_path("bash");
        fs::path tarPath = bp::search_path("tar");
        fs::path findPath = bp::search_path("find");

        stringstream ssCmd;
        ssCmd << bashPath.string() << " -c \"" << findPath.string() << " \\\"" << logDir.string()
              << "\\\" -name \\\"*.log\\\" | " << tarPath.string() << " -czf \\\"" << filePath.string()
              << "\\\" -T -\"";

        string output;
        execute(ssCmd.str(), chrono::seconds(60), &output);

        pushBinaryAttachmentCmd(filePath.string(), fileName.string(), cmdGET_LOG, _sender.m_ID);
    }
    catch (exception& e)
    {
        LOG(error) << e.what();
        pushMsg<cmdREPLY>(SReplyCmd(e.what(), (uint16_t)SReplyCmd::EStatusCode::ERROR, 0, cmdGET_LOG));
    }

    return true;
}

void CCommanderChannel::readAgentIDFile()
{
    const string sAgentIDFile(CUserDefaults::getAgentIDFilePath());
    LOG(info) << "Reading an agent ID file: " << sAgentIDFile;
    ifstream f(sAgentIDFile.c_str());
    if (!f.is_open() || !f.good())
    {
        string msg("Could not open an agent ID file: ");
        msg += sAgentIDFile;
        throw runtime_error(msg);
    }
    f >> m_id;
}

void CCommanderChannel::createAgentIDFile() const
{
    const string sAgentIDFile(CUserDefaults::getAgentIDFilePath());
    LOG(info) << "Creating an agent ID file: " << sAgentIDFile;
    ofstream f(sAgentIDFile.c_str());
    if (!f.is_open() || !f.good())
    {
        string msg("Could not open an agent ID file: ");
        msg += sAgentIDFile;
        throw runtime_error(msg);
    }

    f << m_id;
}

void CCommanderChannel::deleteAgentIDFile() const
{
    const string sAgentIDFile(CUserDefaults::getAgentIDFilePath());
    if (sAgentIDFile.empty())
        return;

    // TODO: check error code
    unlink(sAgentIDFile.c_str());
}

bool CCommanderChannel::on_cmdASSIGN_USER_TASK(SCommandAttachmentImpl<cmdASSIGN_USER_TASK>::ptr_t _attachment,
                                               SSenderInfo& _sender)
{
    LOG(info) << "Received a user task assignment. " << *_attachment;

    // Check that topology is the same before task assignment
    try
    {
        if (m_topo.getHash() != _attachment->m_topoHash)
        {
            stringstream ss;
            ss << "Topology hash check failed: " << m_topo.getHash() << " (must be " << _attachment->m_topoHash << ")";
            throw runtime_error(ss.str());
        }
    }
    catch (exception& _e)
    {
        pushMsg<cmdREPLY>(SReplyCmd(_e.what(), (uint16_t)SReplyCmd::EStatusCode::ERROR, 0, cmdASSIGN_USER_TASK),
                          _sender.m_ID);
        LOG(error) << "Assign task error: " << _e.what();
        return true;
    }

    SSlotInfo* slot;
    try
    {
        slot = &(getSlotInfoById(_sender.m_ID));
    }
    catch (exception& _e)
    {
        pushMsg<cmdREPLY>(SReplyCmd(_e.what(), (uint16_t)SReplyCmd::EStatusCode::ERROR, 0, cmdASSIGN_USER_TASK),
                          _sender.m_ID);
        LOG(error) << "Assign task error: " << _e.what();
        return true;
    }

    slot->m_sUsrExe = _attachment->m_sExeFile;
    slot->m_sUsrEnv = _attachment->m_sEnvFile;
    slot->m_taskID = _attachment->m_taskID;
    slot->m_taskIndex = _attachment->m_taskIndex;
    slot->m_collectionIndex = _attachment->m_collectionIndex;
    slot->m_taskPath = _attachment->m_taskPath;
    slot->m_groupName = _attachment->m_groupName;
    slot->m_collectionName = _attachment->m_collectionName;
    slot->m_taskName = _attachment->m_taskName;

    {
        lock_guard<std::mutex> lock(m_taskIDToSlotIDMapMutex);
        m_taskIDToSlotIDMap.insert(make_pair(slot->m_taskID, slot->m_id));
    }

    // Replace all %taskIndex% and %collectionIndex% in executable path with their values.
    boost::algorithm::replace_all(slot->m_sUsrExe, "%taskIndex%", to_string(slot->m_taskIndex));
    if (slot->m_collectionIndex != numeric_limits<uint32_t>::max())
        boost::algorithm::replace_all(slot->m_sUsrExe, "%collectionIndex%", to_string(slot->m_collectionIndex));

    // If the user task was transfered, than replace "%DDS_DEFAULT_TASK_PATH%" with the real path
    boost::filesystem::path dir(CUserDefaults::instance().getSlotsRootDir());
    dir /= to_string(_sender.m_ID);
    dir += fs::path::preferred_separator;
    boost::algorithm::replace_all(slot->m_sUsrExe, "%DDS_DEFAULT_TASK_PATH%", dir.generic_string());
    // If the user custom environment was transfered, than replace "%DDS_DEFAULT_TASK_PATH%" with the real path
    boost::algorithm::replace_all(slot->m_sUsrEnv, "%DDS_DEFAULT_TASK_PATH%", dir.generic_string());

    try
    {
        string filePath;
        string filename;
        string cmdStr;
        smart_path(&slot->m_sUsrExe);
        parseExe(slot->m_sUsrExe, "", filePath, filename, cmdStr);
        slot->m_sUsrExe = cmdStr;
    }
    catch (exception& _e)
    {
        LOG(error) << _e.what();
        pushMsg<cmdREPLY>(SReplyCmd(_e.what(), (uint16_t)SReplyCmd::EStatusCode::ERROR, 0, cmdASSIGN_USER_TASK),
                          _sender.m_ID);
        return true;
    }

    pushMsg<cmdREPLY>(SReplyCmd("User task assigned", (uint16_t)SReplyCmd::EStatusCode::OK, 0, cmdASSIGN_USER_TASK),
                      _sender.m_ID);

    return true;
}

bool CCommanderChannel::on_cmdACTIVATE_USER_TASK(SCommandAttachmentImpl<cmdACTIVATE_USER_TASK>::ptr_t _attachment,
                                                 SSenderInfo& _sender)
{
    // take slot ID from the command attachment
    auto slot_it = m_slots.find(_attachment->m_id);
    if (slot_it == m_slots.end())
    {
        LOG(error) << "Received activation command. Wrong slot ID.";
        // Send response back to server
        pushMsg<cmdREPLY>(SReplyCmd("Received activation command. Wrong slot ID.",
                                    (uint16_t)SReplyCmd::EStatusCode::ERROR,
                                    0,
                                    cmdACTIVATE_USER_TASK),
                          _sender.m_ID);
        return true;
    }

    const SSlotInfo& slot = slot_it->second;

    // Revoke drain of the write queue to start accept messages
    m_intercomChannel->drainWriteQueue(false, slot.m_id);

    string sUsrExe(slot.m_sUsrExe);

    if (sUsrExe.empty())
    {
        LOG(info) << "Received activation command. Ignoring the command, since no task is assigned.";
        // Send response back to server
        pushMsg<cmdREPLY>(SReplyCmd("No task is assigned. Activation is ignored.",
                                    (uint16_t)SReplyCmd::EStatusCode::OK,
                                    0,
                                    cmdACTIVATE_USER_TASK),
                          _sender.m_ID);
        return true;
    }

    StringVector_t params;
    pid_t pidUsrTask(0);

    try
    {
        // set task's environment
        LOG(info) << "Setting up task's environment: "
                  << "DDS_TASK_ID:" << slot.m_taskID << " DDS_TASK_INDEX:" << slot.m_taskIndex
                  << " DDS_COLLECTION_INDEX:" << slot.m_collectionIndex << " DDS_TASK_PATH:" << slot.m_taskPath
                  << " DDS_GROUP_NAME:" << slot.m_groupName << " DDS_COLLECTION_NAME:" << slot.m_collectionName
                  << " DDS_TASK_NAME:" << slot.m_taskName << " DDS_SLOT_ID:" << slot.m_id
                  << " DDS_SESSION_ID: " << dds::env_prop<dds::EEnvProp::dds_session_id>();
        if (::setenv("DDS_TASK_ID", to_string(slot.m_taskID).c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_TASK_ID");
        if (::setenv("DDS_TASK_INDEX", to_string(slot.m_taskIndex).c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_TASK_INDEX");
        if (slot.m_collectionIndex != numeric_limits<uint32_t>::max())
            if (::setenv("DDS_COLLECTION_INDEX", to_string(slot.m_collectionIndex).c_str(), 1) == -1)
                throw MiscCommon::system_error("Failed to set up $DDS_COLLECTION_INDEX");
        if (::setenv("DDS_TASK_PATH", slot.m_taskPath.c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_TASK_PATH");
        if (::setenv("DDS_GROUP_NAME", slot.m_groupName.c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_GROUP_NAME");
        if (::setenv("DDS_COLLECTION_NAME", slot.m_collectionName.c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_COLLECTION_NAME");
        if (::setenv("DDS_TASK_NAME", slot.m_taskName.c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_TASK_NAME");
        if (::setenv("DDS_SLOT_ID", to_string(slot.m_id).c_str(), 1) == -1)
            throw MiscCommon::system_error("Failed to set up $DDS_SLOT_ID");

        dispatchHandlers<>(EChannelEvents::OnAssignUserTask, _sender);

        // execute the task
        LOG(info) << "Executing user task: " << sUsrExe;

        // Task output files: <user_task_name>_<datetime>_<task_id>_<out/err>.log
        stringstream ssTaskOutput;
        ssTaskOutput << CUserDefaults::getDDSPath() << slot.m_taskName;

        // TODO: Change the code below once on gcc5+. GCC 4.9 doesn't support put_time
        //
        // current time
        // auto now = std::chrono::system_clock::now();
        // auto in_time_t = std::chrono::system_clock::to_time_t(now);
        // ssTaskOutput << std::put_time(std::localtime(&in_time_t), "_%F_%H-%M-%S");

        time_t now = chrono::system_clock::to_time_t(chrono::system_clock::now());
        struct tm* ptm = localtime(&now);
        char buffer[20];
        strftime(buffer, 20, "%Y-%m-%d-%H-%M-%S", ptm);
        ssTaskOutput << "_" << buffer;

        // task id
        ssTaskOutput << "_" << slot.m_taskID;

        string sTaskStdOut(ssTaskOutput.str() + "_out.log");
        string sTaskStdErr(ssTaskOutput.str() + "_err.log");

        boost::filesystem::path pathSlotDir(CUserDefaults::instance().getSlotsRootDir());
        pathSlotDir /= to_string(_sender.m_ID);
        const boost::filesystem::path pathTaskWrapperIn("dds_user_task_wrapper.sh.in");
        const boost::filesystem::path pathTaskWrapper(pathSlotDir / "dds_user_task_wrapper.sh");

        // Replace placeholders in the task wrapper
        fs::ifstream fTaskWrapper(pathTaskWrapperIn.native());
        if (!fTaskWrapper.is_open())
            throw runtime_error("Failed to open task wrapper template.");
        string sTaskWrapperContent((istreambuf_iterator<char>(fTaskWrapper)), istreambuf_iterator<char>());
        // JOB SCRIPT ---  Custom environment
        if (!slot.m_sUsrEnv.empty())
        {
            boost::replace_all(sTaskWrapperContent, "# %DDS_USER_ENVIRONMENT%", "source " + slot.m_sUsrEnv);
        }
        // JOB SCRIPT --- Task Executable
        boost::replace_all(sTaskWrapperContent, "# %DDS_USER_TASK%", sUsrExe);

        fs::ofstream fTaskWrapperOut(pathTaskWrapper.native());
        if (!fTaskWrapperOut.is_open())
            throw runtime_error("Failed to create task wrapper script.");

        fTaskWrapperOut << sTaskWrapperContent;
        fTaskWrapperOut.flush();
        fTaskWrapperOut.close();

        // Apply execute access on the wrapper script
        boost::filesystem::permissions(pathTaskWrapper, boost::filesystem::add_perms | boost::filesystem::owner_all);

        stringstream ssCmd;
        ssCmd << boost::process::search_path("bash").string();
        ssCmd << " -c \" " << pathTaskWrapper.native() << " \"";

        pidUsrTask = execute(ssCmd.str(), sTaskStdOut, sTaskStdErr);
    }
    catch (exception& _e)
    {
        LOG(error) << _e.what();
        // Send response back to server
        pushMsg<cmdREPLY>(SReplyCmd(_e.what(), (uint16_t)SReplyCmd::EStatusCode::ERROR, 0, cmdACTIVATE_USER_TASK),
                          _sender.m_ID);
        return true;
    }

    stringstream ss;
    ss << "User task (pid:" << pidUsrTask << ") is activated.";
    LOG(info) << ss.str();

    onNewUserTask(slot.m_id, pidUsrTask);

    // Send response back to server
    pushMsg<cmdREPLY>(SReplyCmd(ss.str(), (uint16_t)SReplyCmd::EStatusCode::OK, 0, cmdACTIVATE_USER_TASK),
                      _sender.m_ID);

    return true;
}

bool CCommanderChannel::on_cmdSTOP_USER_TASK(SCommandAttachmentImpl<cmdSTOP_USER_TASK>::ptr_t _attachment,
                                             SSenderInfo& _sender)
{
    try
    {
        auto& slot = getSlotInfoById(_sender.m_ID);

        if (slot.m_taskID == 0)
        {
            // No running tasks, nothing to stop
            // Send response back to server
            pushMsg<cmdREPLY>(SReplyCmd("No tasks is running. Nothing to stop.",
                                        (uint16_t)SReplyCmd::EStatusCode::OK,
                                        0,
                                        cmdSTOP_USER_TASK),
                              _sender.m_ID);
            return true;
        }

        if (slot.m_pid > 0)
            terminateChildrenProcesses(slot.m_pid);
    }
    catch (exception& _e)
    {
        LOG(warning) << "Can't find user task on slot " << _sender.m_ID << ": " << _e.what();
    }

    pushMsg<cmdREPLY>(SReplyCmd("Done", (uint16_t)SReplyCmd::EStatusCode::OK, 0, cmdSTOP_USER_TASK), _sender.m_ID);
    return true;
}

bool CCommanderChannel::on_cmdADD_SLOT(SCommandAttachmentImpl<cmdADD_SLOT>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(info) << "Received a ADD SLOT request, id = " << _attachment->m_id;

    // Add new Task slot
    SSlotInfo info;
    info.m_id = _attachment->m_id;

    // TODO: catch exception if directory can not be created
    fs::path dir(CUserDefaults::instance().getSlotsRootDir());
    dir /= to_string(info.m_id);
    fs::create_directories(dir);

    // Add shared memory output for intercom API task
    m_intercomChannel->addOutput(info.m_id, CUserDefaults::instance().getSMLeaderOutputName(info.m_id));

    {
        std::lock_guard<std::mutex> lock(m_mutexSlots);
        m_slots.insert(make_pair(info.m_id, info));
    }

    // Confirm to commander the new slot
    SIDCmd msg_cmd;
    msg_cmd.m_id = info.m_id;
    pushMsg<cmdREPLY_ADD_SLOT>(msg_cmd);

    return true;
}

bool CCommanderChannel::on_cmdUPDATE_KEY(SCommandAttachmentImpl<cmdUPDATE_KEY>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(debug) << "Received key value update: " << *_attachment;

    // Forward message to user task
    m_intercomChannel->pushMsg<cmdUPDATE_KEY>(*_attachment, _sender.m_ID, _sender.m_ID);

    return true;
}

bool CCommanderChannel::on_cmdCUSTOM_CMD(SCommandAttachmentImpl<cmdCUSTOM_CMD>::ptr_t _attachment, SSenderInfo& _sender)
{
    LOG(debug) << "Received custom command: " << *_attachment;

    // Forward message to user task
    m_intercomChannel->pushMsg<cmdCUSTOM_CMD>(*_attachment, _sender.m_ID, _sender.m_ID);

    return true;
}

void CCommanderChannel::onNewUserTask(uint64_t _slotID, pid_t _pid)
{
    // watchdog
    LOG(info) << "Adding user task on slot " << _slotID << " with pid " << _pid << " to tasks queue";
    try
    {
        // Add a new task to the watchdog list
        auto& slot = getSlotInfoById(_slotID);
        slot.m_pid = _pid;
    }
    catch (exception& _e)
    {
        LOG(fatal) << "Can't add new user task on slot " << _slotID << " to the list of children: " << _e.what();
    }

    // Register the user task's watchdog

    LOG(info) << "Starting the watchdog for user task on slot " << _slotID << " pid = " << _pid;

    auto self(shared_from_this());
    CMonitoringThread::instance().registerCallbackFunction(
        [this, self, _slotID, _pid]() -> bool {
            // Send commander server the watchdog heartbeat.
            // It indicates that the agent is executing a task and is not idle
            pushMsg<cmdWATCHDOG_HEARTBEAT>();
            CMonitoringThread::instance().updateIdle();

            try
            {
                // NOTE: We don't use boost::process because it returned an evaluated exit status, but we need a raw to
                // be able to detect how exactly the child exited.
                // boost::process  only checks that the child ended because of a call to ::exit() and does not check for
                // exiting via signal (WIFSIGNALED()).

                // We must call "wait" to check exist status of a child process, otherwise we will crate a
                // zombie :)
                int status;
                pid_t ret = ::waitpid(_pid, &status, WNOHANG | WUNTRACED);
                if (ret < 0)
                {
                    switch (errno)
                    {
                        case ECHILD:
                            LOG(MiscCommon::error) << "Watchdog " << _slotID
                                                   << ": The process or process group specified by pid "
                                                      "does not exist or is not a child of the calling process.";
                            break;
                        case EFAULT:
                            LOG(MiscCommon::error) << "Watchdog " << _slotID << ": stat_loc is not a writable address.";
                            break;
                        case EINTR:
                            LOG(MiscCommon::error) << "Watchdog " << _slotID
                                                   << ": The function was interrupted by a signal. The "
                                                      "value of the location pointed to by stat_loc is undefined.";
                            break;
                        case EINVAL:
                            LOG(MiscCommon::error) << "Watchdog " << _slotID << ": The options argument is not valid.";
                            break;
                        case ENOSYS:
                            LOG(MiscCommon::error) << "Watchdog " << _slotID
                                                   << ": pid specifies a process group (0 or less than "
                                                      "-1), which is not currently supported.";
                            break;
                    }
                    LOG(info) << "User Tasks on slot " << _slotID
                              << " cannot be found. Probably it has exited. pid = " << _pid;
                    LOG(info) << "Stopping the watchdog for user task " << _slotID << " pid = " << _pid;

                    taskExited(_slotID, 0);

                    return false;
                }
                else if (ret == _pid)
                {
                    if (WIFEXITED(status))
                        LOG(info) << "User task on slot " << _slotID << " exited"
                                  << (WCOREDUMP(status) ? " and dumped core" : "") << " with status "
                                  << WEXITSTATUS(status);
                    else if (WIFSTOPPED(status))
                        LOG(info) << "User task on slot " << _slotID << " stopped by signal " << WSTOPSIG(status);
                    else if (WIFSIGNALED(status))
                        LOG(info) << "User task on slot " << _slotID << " killed by signal " << WTERMSIG(status)
                                  << (WCOREDUMP(status) ? "; (core dumped)" : "");
                    else
                        LOG(info) << "User task on slot " << _slotID << " exited with unexpected status: " << status;

                    LOG(info) << "Stopping the watchdog for user task " << _slotID << " pid = " << _pid;

                    LOG(info) << "slot = " << _slotID << " pid = " << _pid
                              << " - done; exit status = " << WEXITSTATUS(status);

                    taskExited(_slotID, status);
                    return false;
                }
            }
            catch (exception& _e)
            {
                LOG(fatal) << "User processe monitoring thread received an exception: " << _e.what();
            }

            return true;
        },
        std::chrono::seconds(5));

    LOG(info) << "Watchdog for task on slot " << _slotID << " pid = " << _pid << " has been registered.";
}

void CCommanderChannel::terminateChildrenProcesses(pid_t _parentPid)
{
    // terminate all child processes of the given parent
    // Either tasks or all processes of the agent.
    pid_t mainPid((_parentPid > 0) ? _parentPid : getpid());

    LOG(info) << "Getting a list of child processes of the agent with pid " << mainPid;
    std::vector<std::string> vecChildren;
    try
    {
        // a pgrep command is used to find out the list of child processes of the task
        stringstream ssCmd;
        ssCmd << boost::process::search_path("pgrep").string() << " -P " << mainPid;
        string output;
        execute(ssCmd.str(), std::chrono::seconds(5), &output);
        boost::split(vecChildren, output, boost::is_any_of(" \n"), boost::token_compress_on);
        vecChildren.erase(
            remove_if(vecChildren.begin(), vecChildren.end(), [](const string& _val) { return _val.empty(); }),
            vecChildren.end());
    }
    catch (...)
    {
    }
    string sChildren;
    for (const auto i : vecChildren)
    {
        if (!sChildren.empty())
            sChildren += ", ";
        sChildren += i;
    }
    LOG(info) << "terminateChildrenProcesses: found " << vecChildren.size() << " child process(es)"
              << (sChildren.empty() ? "." : " " + sChildren);

    LOG(info) << "Sending graceful terminate signal to child processes.";
    for (const auto i : vecChildren)
    {
        LOG(info) << "Sending graceful terminate signal to child process " << i;
        kill(stol(i), SIGTERM);
    }

    LOG(info) << "Wait for tasks to exit...";

    // wait 10 seconds
    for (size_t i = 0; i < 10; ++i)
    {
        for (auto const& pid : vecChildren)
        {
            long lpid = stol(pid);
            LOG(info) << "Waiting for pid = " << lpid;
            if (lpid > 0 && IsProcessRunning(lpid))
                continue;
        }
        // TODO: Needs to be fixed! Implement time-function based timeout measurements
        // instead
        this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // kill all child process of tasks if there are any
    // We do it before terminating tasks to give parenrt task processes a change to read state of children - otherwise
    // we will get zombies if user tasks don't manage their children properly
    for (auto const& pid : vecChildren)
    {
        if (!IsProcessRunning(stol(pid)))
            continue;

        LOG(info) << "Child process with pid = " << pid << " will be forced to exit...";
        kill(stol(pid), SIGKILL);
    }
}

void CCommanderChannel::taskExited(uint64_t _slotID, int _exitCode)
{
    // remove pid from the active children list
    try
    {
        // Add a new task to the watchdog list
        auto& slot = getSlotInfoById(_slotID);

        SUserTaskDoneCmd cmd;
        cmd.m_exitCode = _exitCode;
        cmd.m_taskID = slot.m_taskID;
        pushMsg<cmdUSER_TASK_DONE>(cmd, _slotID);

        {
            lock_guard<std::mutex> lock(m_taskIDToSlotIDMapMutex);
            m_taskIDToSlotIDMap.erase(slot.m_taskID);
        }

        slot.m_pid = 0;
        slot.m_taskID = 0;

        // Drainning the Intercom write queue
        m_intercomChannel->drainWriteQueue(true, _slotID);
    }
    catch (exception& _e)
    {
        LOG(fatal) << "Failed to remove user task on slot " << _slotID << " from the list of children: " << _e.what();
        LOG(error) << "Can't send TASK_DONE. The coresponding slot is missing";
    }
}

void CCommanderChannel::stopChannel()
{
    // terminate external children processes (like user tasks, for example)
    terminateChildrenProcesses(0);

    if (m_intercomChannel)
        m_intercomChannel->stop();

    // stop channel
    stop();
}

void CCommanderChannel::send_cmdUPDATE_KEY(const SSenderInfo& _sender,
                                           SCommandAttachmentImpl<cmdUPDATE_KEY>::ptr_t _attachment)
{
    LOG(debug) << "Received a key update notifications: " << *_attachment;

    try
    {
        string propertyName(_attachment->m_propertyName);
        uint64_t taskID(_attachment->m_senderTaskID);

        auto task = m_topo.getRuntimeTaskById(taskID).m_task;
        auto property = task->getProperty(propertyName);
        // Property doesn't exists for task
        if (property == nullptr)
        {
            stringstream ss;
            ss << "Can't propagate property <" << propertyName << "> that doesn't exist for task <" << task->getName()
               << ">";
            m_intercomChannel->pushMsg<cmdSIMPLE_MSG>(
                SSimpleMsgCmd(ss.str(), MiscCommon::error, cmdUPDATE_KEY), _sender.m_ID, _sender.m_ID);
            return;
        }
        // Cant' propagate property with read access type
        if (property->getAccessType() == CTopoProperty::EAccessType::READ)
        {
            stringstream ss;
            ss << "Can't propagate property <" << property->getName() << "> which has a READ access type for task <"
               << task->getName() << ">";
            m_intercomChannel->pushMsg<cmdSIMPLE_MSG>(
                SSimpleMsgCmd(ss.str(), MiscCommon::error, cmdUPDATE_KEY), _sender.m_ID, _sender.m_ID);
            return;
        }
        // Can't send property with a collection scope if a task is outside a collection
        if ((property->getScopeType() == CTopoProperty::EScopeType::COLLECTION) &&
            (task->getParent()->getType() != CTopoBase::EType::COLLECTION))
        {
            stringstream ss;
            ss << "Can't propagate property <" << property->getName()
               << "> which has a COLLECTION scope type but task <" << task->getName() << "> is not in any collection";
            m_intercomChannel->pushMsg<cmdSIMPLE_MSG>(
                SSimpleMsgCmd(ss.str(), MiscCommon::error, cmdUPDATE_KEY), _sender.m_ID, _sender.m_ID);
            return;
        }

        STopoRuntimeTask::FilterIteratorPair_t taskIt =
            m_topo.getRuntimeTaskIteratorForPropertyName(propertyName, taskID);

        for (auto it = taskIt.first; it != taskIt.second; ++it)
        {
            uint64_t receiverTaskID(it->first);

            // Dont't send message to itself
            if (taskID == receiverTaskID)
                continue;

            auto task = m_topo.getRuntimeTaskById(receiverTaskID).m_task;
            auto property = task->getProperty(propertyName);
            if (property != nullptr && (property->getAccessType() == CTopoProperty::EAccessType::READ ||
                                        property->getAccessType() == CTopoProperty::EAccessType::READWRITE))
            {
                SUpdateKeyCmd cmd;
                cmd.m_propertyName = propertyName;
                cmd.m_value = _attachment->m_value;
                cmd.m_receiverTaskID = receiverTaskID;
                cmd.m_senderTaskID = taskID;

                bool localPush(true);
                uint64_t slotID(0);
                {
                    std::lock_guard<std::mutex> lock(m_taskIDToSlotIDMapMutex);
                    auto it = m_taskIDToSlotIDMap.find(receiverTaskID);
                    localPush = (it != m_taskIDToSlotIDMap.end());
                    slotID = it->second;
                }

                if (localPush)
                {
                    LOG(debug) << "Push update key via shared memory: cmd=<" << cmd << ">; slotID=" << slotID;
                    m_intercomChannel->pushMsg<cmdUPDATE_KEY>(cmd, slotID, slotID);
                }
                else
                {
                    LOG(debug) << "Push update key via network channel: <" << cmd
                               << ">; protocolHeaderID=" << _sender.m_ID;
                    this->pushMsg<cmdUPDATE_KEY>(cmd, _sender.m_ID);
                }

                LOG(debug) << "Property update from agent channel: <" << cmd << ">";
            }
        }
    }
    catch (exception& _e)
    {
        LOG(error) << "Failed to update key: " << _e.what();
    }
}

bool CCommanderChannel::on_cmdUSER_TASK_DONE(SCommandAttachmentImpl<cmdUSER_TASK_DONE>::ptr_t _attachment,
                                             SSenderInfo& _sender)
{
    LOG(debug) << "Received user task done: " << *_attachment;

    // Forward message to user task
    // WORKAROUND: to prevent locking the container on msg push, we create a tmp container with slot IDs
    vector<slotId_t> slotsTmp;
    {
        std::lock_guard<std::mutex> lock(m_mutexSlots);
        std::transform(m_slots.begin(),
                       m_slots.end(),
                       back_inserter(slotsTmp),
                       boost::bind(&SSlotInfo::container_t::value_type::first, _1));
    }

    for (const auto& i : slotsTmp)
    {
        m_intercomChannel->pushMsg<cmdUSER_TASK_DONE>(*_attachment, i, i);
    }

    return true;
}
