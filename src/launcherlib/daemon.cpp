/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (directui@nokia.com)
**
** This file is part of applauncherd
**
** If you have questions regarding the use of this file, please contact
** Nokia at directui@nokia.com.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include "daemon.h"
#include "logger.h"
#include "connection.h"
#include "booster.h"
#include "singleinstance.h"
#include "socketmanager.h"

#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <glob.h>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <systemd/sd-daemon.h>

#include "coverage.h"

// Environment
extern char ** environ;

Daemon * Daemon::m_instance = NULL;
const int Daemon::m_boosterSleepTime = 2;

const std::string Daemon::m_stateDir = std::string(getenv("XDG_RUNTIME_DIR"))+"/applauncherd";
const std::string Daemon::m_stateFile = Daemon::m_stateDir + "/saved-state";

static void sigChldHandler(int)
{
    char v = SIGCHLD;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

static void sigTermHandler(int)
{
    char v = SIGTERM;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

static void sigUsr1Handler(int)
{
    char v = SIGUSR1;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

static void sigUsr2Handler(int)
{
    char v = SIGUSR2;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

static void sigPipeHandler(int)
{
    char v = SIGPIPE;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

static void sigHupHandler(int)
{
    char v = SIGHUP;
    write(Daemon::instance()->sigPipeFd(), &v, 1);
}

Daemon::Daemon(int & argc, char * argv[]) :
    m_daemon(false),
    m_debugMode(false),
    m_bootMode(false),
    m_boosterPid(0),
    m_socketManager(new SocketManager),
    m_singleInstance(new SingleInstance),
    m_reExec(false),
    m_notifySystemd(false),
    m_booster(0)
{
    // Open the log
    Logger::openLog(argc > 0 ? argv[0] : "booster");
    Logger::logDebug("starting..");

    // Install signal handlers. The original handlers are saved
    // in the daemon instance so that they can be restored in boosters.
    setUnixSignalHandler(SIGCHLD, sigChldHandler); // reap zombies
    setUnixSignalHandler(SIGTERM, sigTermHandler); // exit launcher
    setUnixSignalHandler(SIGUSR1, sigUsr1Handler); // enter normal mode from boot mode
    setUnixSignalHandler(SIGUSR2, sigUsr2Handler); // enter boot mode (same as --boot-mode)
    setUnixSignalHandler(SIGPIPE, sigPipeHandler); // broken invoker's pipe
    setUnixSignalHandler(SIGHUP,  sigHupHandler);  // re-exec

    if (!Daemon::m_instance)
    {
        Daemon::m_instance = this;
    }
    else
    {
        throw std::runtime_error("Daemon: Daemon already created!\n");
    }

    // Parse arguments
    parseArgs(ArgVect(argv, argv + argc));

    if (m_reExec)
    {
        restoreState();
    }

    // Store arguments list
    m_initialArgv = argv;
    m_initialArgc = argc;

    if (!m_reExec && socketpair(AF_UNIX, SOCK_DGRAM, 0, m_boosterLauncherSocket) == -1)
    {
        throw std::runtime_error("Daemon: Creating a socket pair for boosters failed!\n");
    }

    if (!m_reExec && pipe(m_sigPipeFd) == -1)
    {
        throw std::runtime_error("Daemon: Creating a pipe for Unix signals failed!\n");
    }

    // Daemonize if desired
    if (m_daemon)
    {
        daemonize();
    }
}

Daemon * Daemon::instance()
{
    return Daemon::m_instance;
}

void Daemon::run(Booster *booster)
{
    m_booster = booster;

    // Make sure that LD_BIND_NOW does not prevent dynamic linker to
    // use lazy binding in later dlopen() calls.
    unsetenv("LD_BIND_NOW");

    // dlopen single-instance
    loadSingleInstancePlugin();

    if (m_reExec)
    {
        // Reap dead booster processes and restart them
        // Note: this cannot be done before booster plugins have been loaded
        reapZombies();
    }
    else
    {
        // Create socket for the booster
        Logger::logDebug("Daemon: initing socket: %s", booster->boosterType().c_str());
        m_socketManager->initSocket(booster->boosterType());

        // Fork each booster for the first time
        Logger::logDebug("Daemon: forking booster: %s", booster->boosterType().c_str());
        forkBooster();
    }

    // Notify systemd that init is done
    if (m_notifySystemd) {
        Logger::logDebug("Daemon: initialization done. Notify systemd\n");
        sd_notify(0, "READY=1");
    }

    // Main loop
    while (true)
    {
        // Variables used by the select call
        fd_set rfds;
        int ndfs = 0;

        // Init data for select
        FD_ZERO(&rfds);

        FD_SET(m_boosterLauncherSocket[0], &rfds);
        ndfs = std::max(ndfs, m_boosterLauncherSocket[0]);

        FD_SET(m_sigPipeFd[0], &rfds);
        ndfs = std::max(ndfs, m_sigPipeFd[0]);

        // Wait for something appearing in the pipes.
        if (select(ndfs + 1, &rfds, NULL, NULL, NULL) > 0)
        {
            Logger::logDebug("Daemon: select done.");

            // Check if a booster died
            if (FD_ISSET(m_boosterLauncherSocket[0], &rfds))
            {
                Logger::logDebug("Daemon: FD_ISSET(m_boosterLauncherSocket[0])");
                readFromBoosterSocket(m_boosterLauncherSocket[0]);
            }

            // Check if we got SIGCHLD, SIGTERM, SIGUSR1 or SIGUSR2
            if (FD_ISSET(m_sigPipeFd[0], &rfds))
            {
                Logger::logDebug("Daemon: FD_ISSET(m_sigPipeFd[0])");
                char dataReceived;
                read(m_sigPipeFd[0], &dataReceived, 1);

                switch (dataReceived)
                {
                case SIGCHLD:
                    Logger::logDebug("Daemon: SIGCHLD received.");
                    reapZombies();
                    break;

                case SIGTERM:
                    Logger::logDebug("Daemon: SIGTERM received.");
                    exit(EXIT_SUCCESS);
                    break;

                case SIGUSR1:
                    Logger::logDebug("Daemon: SIGUSR1 received.");
                    enterNormalMode();
                    break;

                case SIGUSR2:
                    Logger::logDebug("Daemon: SIGUSR2 received.");
                    enterBootMode();
                    break;

                case SIGPIPE:
                    Logger::logDebug("Daemon: SIGPIPE received.");
                    break;

                case SIGHUP:
                    Logger::logDebug("Daemon: SIGHUP received.");
                    reExec();

                    // not reached if re-exec successful
                    break;
                    

                default:
                    break;
                }
            }
        }
    }
}

void Daemon::readFromBoosterSocket(int fd)
{
    pid_t invokerPid = 0;
    int delay        = 0;
    struct msghdr   msg;
    struct cmsghdr *cmsg;
    struct iovec    iov[2];
    char buf[CMSG_SPACE(sizeof(int))];

    iov[0].iov_base = &invokerPid;
    iov[0].iov_len  = sizeof(pid_t);
    iov[1].iov_base = &delay;
    iov[1].iov_len  = sizeof(int);

    msg.msg_iov        = iov;
    msg.msg_iovlen     = 2;
    msg.msg_name       = NULL;
    msg.msg_namelen    = 0;
    msg.msg_control    = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg(fd, &msg, 0) >= 0)
    {
        Logger::logDebug("Daemon: invoker's pid: %d\n", invokerPid);
        Logger::logDebug("Daemon: respawn delay: %d \n", delay);
        if (invokerPid != 0)
        {
            // Store booster - invoker pid pair
            // Store booster - invoker socket pair
            if (m_boosterPid)
            {
                cmsg = CMSG_FIRSTHDR(&msg);
                int newFd;                 
                memcpy(&newFd, CMSG_DATA(cmsg), sizeof(int));
                Logger::logDebug("Daemon: socket file descriptor: %d\n", newFd);
                m_boosterPidToInvokerPid[m_boosterPid] = invokerPid;
                m_boosterPidToInvokerFd[m_boosterPid] = newFd;
            }
        }
    }
    else
    {
        Logger::logError("Daemon: Nothing read from the socket\n");
        // Critical error communicating with booster. Exiting applauncherd.
        _exit(EXIT_FAILURE);
    }

    // 2nd param guarantees some time for the just launched application
    // to start up before forking new booster. Not doing this would
    // slow down the start-up significantly on single core CPUs.

    forkBooster(delay);
}

void Daemon::killProcess(pid_t pid, int signal) const
{
    if (pid > 0)
    {
        Logger::logDebug("Daemon: Killing pid %d with %d", pid, signal);
        if (kill(pid, signal) != 0)
        {
            Logger::logError("Daemon: Failed to kill %d: %s\n",
                             pid, strerror(errno));
        }
    }
}

void Daemon::loadSingleInstancePlugin()
{
    void * handle = dlopen(SINGLE_INSTANCE_PATH, RTLD_NOW);
    if (!handle)
    {
        Logger::logWarning("Daemon: dlopening single-instance failed: %s", dlerror());
    }
    else
    {
        if (m_singleInstance->validateAndRegisterPlugin(handle))
        {
            Logger::logDebug("Daemon: single-instance plugin loaded.'");
        }
        else
        {
            Logger::logWarning("Daemon: Invalid single-instance plugin: '%s'",
                               SINGLE_INSTANCE_PATH);
        }
    }
}

void Daemon::forkBooster(int sleepTime)
{
    if (!m_booster) {
        // Critical error unknown booster type. Exiting applauncherd.
        _exit(EXIT_FAILURE);
    }

    // Invalidate current booster pid
    m_boosterPid = 0;

    // Fork a new process
    pid_t newPid = fork();

    if (newPid == -1)
        throw std::runtime_error("Daemon: Forking while invoking");

    if (newPid == 0) /* Child process */
    {
        // Restore used signal handlers
        restoreUnixSignalHandlers();

        // Will get this signal if applauncherd dies
        prctl(PR_SET_PDEATHSIG, SIGHUP);

        // Close unused read end of the booster socket
        close(m_boosterLauncherSocket[0]);

        // Close signal pipe
        close(m_sigPipeFd[0]);
        close(m_sigPipeFd[1]);

        // Close socket file descriptors
        FdMap::iterator i(m_boosterPidToInvokerFd.begin());
        while (i != m_boosterPidToInvokerFd.end())
        {
            if ((*i).second != -1) {
                close((*i).second);
                (*i).second = -1;
            }
            i++;
        }
        // Set session id
        if (setsid() < 0)
            Logger::logError("Daemon: Couldn't set session id\n");

        // Guarantee some time for the just launched application to
        // start up before initializing new booster if needed.
        // Not done if in the boot mode.
        if (!m_bootMode && sleepTime)
            sleep(sleepTime);

        Logger::logDebug("Daemon: Running a new Booster of type '%s'", m_booster->boosterType().c_str());

        // Initialize and wait for commands from invoker
        m_booster->initialize(m_initialArgc, m_initialArgv, m_boosterLauncherSocket[1],
                              m_socketManager->findSocket(m_booster->boosterType().c_str()),
                              m_singleInstance, m_bootMode);

        // Run the current Booster
        int retval = m_booster->run(m_socketManager);

        // Finish
        delete m_booster;

        // _exit() instead of exit() to avoid situation when destructors
        // for static objects may be run incorrectly
        _exit(retval);
    }
    else /* Parent process */
    {
        // Store the pid so that we can reap it later
        m_children.push_back(newPid);

        // Set current process ID globally to the given booster type
        // so that we now which booster to restart when booster exits.
        m_boosterPid = newPid;
    }
}

void Daemon::reapZombies()
{
    // Loop through all child pid's and wait for them with WNOHANG.
    PidVect::iterator i(m_children.begin());
    while (i != m_children.end())
    {
        // Check if the pid had exited and become a zombie
        int status;
        pid_t pid = waitpid(*i, &status, WNOHANG);
        if (pid)
        {
            // The pid had exited. Remove it from the pid vector.
            i = m_children.erase(i);

            // Find out if the exited process has a mapping with an invoker process.
            // If this is the case, then kill the invoker process with the same signal
            // that killed the exited process.
            PidMap::iterator it = m_boosterPidToInvokerPid.find(pid);
            if (it != m_boosterPidToInvokerPid.end())
            {
                Logger::logDebug("Daemon: Terminated process had a mapping to an invoker pid");

                if (WIFEXITED(status))
                {
                    Logger::logInfo("Boosted process (pid=%d) exited with status %d\n", pid, WEXITSTATUS(status));
                    Logger::logDebug("Daemon: child exited by exit(x), _exit(x) or return x\n");
                    Logger::logDebug("Daemon: x == %d\n", WEXITSTATUS(status));
                    FdMap::iterator fd = m_boosterPidToInvokerFd.find(pid);
                    if (fd != m_boosterPidToInvokerFd.end())
                    {
                        write((*fd).second, &INVOKER_MSG_EXIT, sizeof(uint32_t));
                        int exitStatus = WEXITSTATUS(status);
                        write((*fd).second, &exitStatus, sizeof(int));
                        close((*fd).second);
                        m_boosterPidToInvokerFd.erase(fd);
                    }
                }
                else if (WIFSIGNALED(status))
                {
                    int signal = WTERMSIG(status);
                    pid_t invokerPid = (*it).second;

                    Logger::logInfo("Boosted process (pid=%d) was terminated due to signal %d\n", pid, signal);
                    Logger::logDebug("Daemon: Booster (pid=%d) was terminated due to signal %d\n", pid, signal);
                    Logger::logDebug("Daemon: Killing invoker process (pid=%d) by signal %d..\n", invokerPid, signal);

                    FdMap::iterator fd = m_boosterPidToInvokerFd.find(pid);
                    if (fd != m_boosterPidToInvokerFd.end())
                    {
                        close((*fd).second);
                        m_boosterPidToInvokerFd.erase(fd);
                    }

                    killProcess(invokerPid, signal);
                }

                // Remove a dead booster
                m_boosterPidToInvokerPid.erase(it);
            }

            // Check if pid belongs to a booster and restart the dead booster if needed
            if (pid == m_boosterPid)
            {
                forkBooster(m_boosterSleepTime);
            }
        }
        else
        {
            i++;
        }
    }
}

void Daemon::daemonize()
{
    // Our process ID and Session ID
    pid_t pid, sid;

    // Fork off the parent process: first fork
    pid = fork();
    if (pid < 0)
        throw std::runtime_error("Daemon: Unable to fork daemon");

    // If we got a good PID, then we can exit the parent process.
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    // Fork off the parent process: second fork
    pid = fork();
    if (pid < 0)
        throw std::runtime_error("Daemon: Unable to fork daemon");

    // If we got a good PID, then we can exit the parent process.
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    // Change the file mode mask
    umask(0);

    // Open any logs here

    // Create a new SID for the child process
    sid = setsid();
    if (sid < 0)
        throw std::runtime_error("Daemon: Unable to setsid.");

    // Change the current working directory
    if ((chdir("/")) < 0)
        throw std::runtime_error("Daemon: Unable to chdir to '/'");

    // Open file descriptors pointing to /dev/null
    // Redirect standard file descriptors to /dev/null
    // Close new file descriptors

    const int new_stdin = open("/dev/null", O_RDONLY);
    if (new_stdin != -1) {
        dup2(new_stdin, STDIN_FILENO);
        close(new_stdin);
    }

    const int new_stdout = open("/dev/null", O_WRONLY);
    if (new_stdout != -1) {
        dup2(new_stdout, STDOUT_FILENO);
        close(new_stdout);
    }

    const int new_stderr = open("/dev/null", O_WRONLY);
    if (new_stderr != -1) {
        dup2(new_stderr, STDERR_FILENO);
        close(new_stderr);
    }
}

void Daemon::parseArgs(const ArgVect & args)
{
    for (ArgVect::const_iterator i(args.begin() + 1); i != args.end(); i++)
    {
        if ((*i) == "--boot-mode" || (*i) == "-b")
        {
            Logger::logInfo("Daemon: Boot mode set.");
            m_bootMode = true;
        }
        else if ((*i) == "--daemon" || (*i) == "-d")
        {
            m_daemon = true;
        }
        else if ((*i) == "--debug")
        {
            Logger::setDebugMode(true);
            m_debugMode = true;
        }
        else if ((*i) == "--help" || (*i) == "-h")
        {
            usage(args[0].c_str(), EXIT_SUCCESS);
        }
        else if ((*i) == "--re-exec")
        {
            m_reExec = true;
        }
        else if ((*i) == "--systemd")
        {
            m_notifySystemd = true;
        }
        else
        {
            if ((*i).find_first_not_of(' ') != string::npos)
               usage(args[0].c_str(), EXIT_FAILURE);
        }
    }
}

// Prints the usage and exits with given status
void Daemon::usage(const char *name, int status)
{
    printf("\nUsage: %s [options]\n\n"
           "Start the application launcher daemon.\n\n"
           "Options:\n"
           "  -b, --boot-mode  Start %s in the boot mode. This means that\n"
           "                   boosters will not initialize caches and booster\n"
           "                   respawn delay is set to zero.\n"
           "                   Normal mode is restored by sending SIGUSR1\n"
           "                   to the launcher.\n"
           "                   Boot mode can be activated also by sending SIGUSR2\n"
           "                   to the launcher.\n"
           "  -d, --daemon     Run as %s a daemon.\n"
           "  --systemd        Notify systemd when initialization is done\n"
           "  --debug          Enable debug messages and log everything also to stdout.\n"
           "  -h, --help       Print this help.\n\n",
           name, name, name);

    exit(status);
}

int Daemon::sigPipeFd() const
{
    return m_sigPipeFd[1];
}

void Daemon::enterNormalMode()
{
    if (m_bootMode)
    {
        m_bootMode = false;

        // Kill current boosters
        killBoosters();

        Logger::logInfo("Daemon: Exited boot mode.");
    }
    else
    {
        Logger::logInfo("Daemon: Already in normal mode.");
    }
}

void Daemon::enterBootMode()
{
    if (!m_bootMode)
    {
        m_bootMode = true;

        // Kill current boosters
        killBoosters();

        Logger::logInfo("Daemon: Entered boot mode.");
    }
    else
    {
        Logger::logInfo("Daemon: Already in boot mode.");
    }
}

void Daemon::killBoosters()
{
    if (m_boosterPid)
        killProcess(m_boosterPid, SIGTERM);

    // NOTE!!: m_boosterPid must not be cleared
    // in order to automatically start new boosters.
}

void Daemon::setUnixSignalHandler(int signum, sighandler_t handler)
{
    sighandler_t old_handler = signal(signum, handler);

    if (signum == SIGHUP && old_handler == SIG_IGN)
    {
        // SIGHUP is a special case. It is set to SIG_IGN
        // when applauncherd does a re-exec, but we still want the
        // boosters / launched applications to get the default
        // handler.
        m_originalSigHandlers[signum] = SIG_DFL;
    }
    else if (old_handler != SIG_ERR)
    {
        m_originalSigHandlers[signum] = old_handler;
    } 
    else 
    {
        throw std::runtime_error("Daemon: Failed to set signal handler");
    }
}

void Daemon::restoreUnixSignalHandlers()
{
    for (SigHandlerMap::iterator it = m_originalSigHandlers.begin(); it != m_originalSigHandlers.end(); it++ )
    {
        signal(it->first, it->second);
    }

    m_originalSigHandlers.clear();
}


Daemon::~Daemon()
{
    delete m_socketManager;
    delete m_singleInstance;

    Logger::closeLog();
}

void Daemon::reExec()
{
    Logger::logInfo("Daemon: Re-exec requested.");

    struct stat st;
    if (stat(m_stateDir.c_str(), &st) != 0)
    {
        Logger::logDebug("Daemon: State saving directory %s does not exist", m_stateDir.c_str());
        Logger::logDebug("Daemon: Attempting to create it");

        if (mkdir(m_stateDir.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0)
        {
            Logger::logDebug("Daemon: Failed to create directory, re-exec failed, exiting.");
            _exit(1);
        }
    }

    if (stat(m_stateDir.c_str(), &st) != 0)
    {
        Logger::logDebug("Daemon: Directory vanished, re-exec failed, exiting.");
        _exit(1);
    }
    if (!S_ISDIR(st.st_mode))
    {
        Logger::logDebug("Daemon: %s exists but it is not a directory, re-exec failed, exiting.", m_stateDir.c_str());
        _exit(1);
    }

    try {
        std::ofstream ss(m_stateFile.c_str());
        ss.exceptions (std::ifstream::failbit | std::ifstream::badbit);

        // dump the pid to double check that the state file is from this process
        ss << "my-pid " << getpid() << std::endl;

        // Save debug mode first, restoring it will enable debug logging.
        // This way we get debug output from the re-execed daemon as early
        // as possible.
        ss << "debug-mode " << m_debugMode << std::endl;

        // The pids of the dead boosters are also passed as children, but
        // this causes no harm.
        for(PidVect::iterator it = m_children.begin(); it != m_children.end(); it++)
        {
            ss << "child " << *it << std::endl;
        }

        for(PidMap::iterator it = m_boosterPidToInvokerPid.begin(); it != m_boosterPidToInvokerPid.end(); it++)
        {
            ss << "booster-invoker-pid " << it->first << " " << it->second << std::endl;
        }

        for(FdMap::iterator it = m_boosterPidToInvokerFd.begin(); it != m_boosterPidToInvokerFd.end(); it++)
        {
            ss << "booster-invoker-fd " << it->first << " " << it->second << std::endl;
        }

        ss << "booster-pid " << m_boosterPid << std::endl;

        ss << "launcher-socket " << m_boosterLauncherSocket[0] << " " << m_boosterLauncherSocket[1] << std::endl;

        ss << "sigpipe-fd " << m_sigPipeFd[0] << " " << m_sigPipeFd[1] << std::endl;

        ss << "boot-mode " << m_bootMode << std::endl;

        SocketManager::SocketHash s = m_socketManager->getState();
        for(SocketManager::SocketHash::iterator it = s.begin(); it != s.end(); it++)
        {
            ss << "socket-hash " << it->first << " " << it->second << std::endl;
        }

        // when the new applauncherd reads this,
        // it knows state saving was successful.
        ss << "end" << std::endl;
        ss.close();
    }
    catch (std::ofstream::failure e)
    {
        Logger::logError("Daemon: Failed to save state, re-exec failed, exiting.");
        _exit(1);
    }
    
    char* argv[] = { const_cast<char*>("/usr/bin/applauncherd.bin"),
                     const_cast<char*>("--re-exec"),
                     const_cast<char*>("                                                  "),
                     NULL};

    // The boosters have state which will become stale, so kill them.
    // The dead boosters will be reaped when the re-execed applauncherd
    // calls reapZombies after it has initialized.
    killBoosters();

    // Signal handlers are reset at exec(), so we will lose
    // the SIGHUP handling. However, ignoring a signal is preserved
    // over exec(), so start ignoring SIGHUP to prevent applauncherd
    // dying if we receive multiple SIGHUPs.
    signal(SIGHUP, SIG_IGN);

    Logger::logDebug("Daemon: configuration saved succesfully, call execve() ");
    execve(argv[0], argv, environ);

    // Not reached.
    Logger::logDebug("Daemon: Failed to execute execve(),  re-exec failed, exiting.");
    _exit(1);
}

void Daemon::restoreState()
{
    try
    {
        // We have saved state, try to restore it.
        std::ifstream ss(m_stateFile.c_str());
        ss.exceptions (std::ifstream::failbit | std::ifstream::badbit);

        std::string token;

        ss >> token;

        // Bit of defensive programming. Read the pid of the process
        // that left the state file. If it is different from my pid,
        // then something is wrong, and we better exit.
        if (token != "my-pid")
        {
            throw "Daemon: malformed state file, exiting.";
        }
        else
        {
            int pid;
            ss >> pid;
            if (pid != getpid())
            {
                throw "Daemon: stale state file, exiting.";
            }
        }
        
        // Do not check for eof, instead trigger an exception
        // if reading past end of file. When state restore is
        // successful we read the "end" token and return from
        // this method.
        while (true) 
        {
            ss >> token;
            
            if (token == "end")
            {
                // successfully restored state
                ss.close();

                // In debug mode it is better to leave the file there
                // so it can be examined.
                if (!m_debugMode && remove(m_stateFile.c_str()) == -1)
                {
                    Logger::logError("Daemon: could not remove state file %s", m_stateFile.c_str());
                }
                Logger::logDebug("Daemon: state restore completed");
                return;
            } 
            else if (token == "child")
            {
                int arg1;
                ss >> arg1;
                Logger::logDebug("Daemon: restored child %d", arg1);
                m_children.push_back(arg1);
            } 
            else if (token == "booster-invoker-pid")
            {
                int arg1, arg2;
                ss >> arg1;
                ss >> arg2;
                Logger::logDebug("Daemon: restored m_boosterPidToInvokerPid[%d] = %d", arg1, arg2);
                m_boosterPidToInvokerPid[arg1] = arg2;
            } 
            else if (token == "booster-invoker-fd")
            {
                int arg1, arg2;
                ss >> arg1;
                ss >> arg2;
                Logger::logDebug("Daemon: restored m_boosterPidToInvokerFd[%d] = %d", arg1, arg2);
                m_boosterPidToInvokerFd[arg1] = arg2;
            } 
            else if (token == "booster-pid")
            {
                int arg1;
                ss >> arg1;
                Logger::logDebug("Daemon: restored m_boosterPid = %d", arg1);

                m_boosterPid = arg1;
            } 
            else if (token == "launcher-socket")
            {
                int arg1, arg2;
                ss >> arg1;
                ss >> arg2;
                Logger::logDebug("Daemon: restored m_boosterLauncherSocket[] = {%d, %d}", arg1, arg2);
                m_boosterLauncherSocket[0] = arg1;
                m_boosterLauncherSocket[1] = arg2;
            } 
            else if (token == "sigpipe-fd")
            {
                int arg1, arg2;
                ss >> arg1;
                ss >> arg2;
                Logger::logDebug("Daemon: restored m_sigPipeFd[] = {%d, %d}", arg1, arg2);
                m_sigPipeFd[0] = arg1;
                m_sigPipeFd[1] = arg2;
            } 
            else if (token == "socket-hash")
            {
                std::string arg1;
                int arg2;
                ss >> arg1;
                ss >> arg2;
                m_socketManager->addMapping(arg1, arg2);
                Logger::logDebug("Daemon: restored socketHash[%s] = %d", arg1.c_str(), arg2);
            }
            else if (token == "debug-mode")
            {
                bool arg1;
                ss >> arg1;
                m_debugMode = arg1;
                Logger::setDebugMode(m_debugMode);
                Logger::logDebug("Daemon: restored m_debugMode = %d", arg1);
            }
            else if (token == "boot-mode")
            {
                bool arg1;
                ss >> arg1;
                m_bootMode = arg1;
                Logger::logDebug("Daemon: restored m_bootMode = %d", arg1);
            }
        }
    } 
    catch (std::ifstream::failure e)
    {
        // Ran out of saved state before "end" token
        // or there was some other error in restoring the sate.
        Logger::logError("Daemon: Failed to restore saved state, exiting."); 
    }
    catch (char *err)
    {
        // Some other error, e.g. stale state file
        Logger::logError(err);
    }

    // In debug mode it is better to leave the file there
    // so it can be examined.
    if (!m_debugMode && remove(m_stateFile.c_str()) == -1)
    {
        Logger::logError("Daemon: could not remove state file %s", m_stateFile.c_str());
    }

    // This is only reached if state restore was unsuccessful.
    _exit(1);
}
