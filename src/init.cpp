// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"

#include "txdb.h"
#include "net.h"
#include "util.h"
#include "rpcserver.h"
#include "rpcclient.h"
#include "net.h"
#include "state.h"
#include "sync.h"
#include "ui_interface.h"
#include "smessage.h"
#include "ringsig.h"
#include "miner.h"

#ifdef ENABLE_WALLET
#include "db.h"
#include "wallet.h"
#include "walletdb.h"
#endif

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <openssl/crypto.h>

#ifndef WIN32
#include <signal.h>
#endif

namespace fs = boost::filesystem;

using namespace std;
using namespace boost;

//CWallet* pwalletMain;
#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
int nWalletBackups = 10;
#endif
CClientUIInterface uiInterface;

static const bool DEFAULT_STOPAFTERBLOCKIMPORT = false;

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

volatile bool fRequestShutdown = false;
//boost::atomic<bool> fRequestShutdown(false);

void StartShutdown()
{
    fRequestShutdown = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown;
}

static boost::scoped_ptr<ECCVerifyHandle> globalVerifyHandle;

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//bool Finalise()
void Shutdown()
{
	fRequestShutdown = true; // Needed when we shutdown the wallet
    LogPrintf("Shutdown : In progress...\n");
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown) return;
	
    //LogPrintf("Finalise()\n");
    
    RenameThread("procurrency-shutoff");
    mempool.AddTransactionsUpdated(1);
	StopRPCThreads();
    SecureMsgShutdown();
	
#ifdef ENABLE_WALLET
	ShutdownRPCMining();	
    if (pwalletMain)
        bitdb.Flush(false);
#endif	
    
    StopNode();
	UnregisterNodeSignals(GetNodeSignals());
    /*DumpMasternodes();*/ //TODO
    
#ifdef ENABLE_WALLET	
    if (pwalletMain)
    {
        {
            LOCK2(cs_main, pwalletMain->cs_wallet);
            pwalletMain->Finalise();
        }
        bitdb.Flush(true);
        UnregisterWallet(pwalletMain);
        //delete pwalletMain;
        //pwalletMain = NULL;
    };
#endif
	
    finaliseRingSigs();
    
    if (nNodeMode == NT_FULL)
    {
        std::map<uint256, CBlockIndex*>::iterator it;

        for (it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
        {
            delete it->second;
        };
        mapBlockIndex.clear();
        if (fDebug)
            LogPrintf("mapBlockIndex cleared.\n");
    } else
    {
        std::map<uint256, CBlockThinIndex*>::iterator it;

        for (it = mapBlockThinIndex.begin(); it != mapBlockThinIndex.end(); ++it)
        {
            delete it->second;
        };
        mapBlockThinIndex.clear();
        if (fDebug)
            LogPrintf("mapBlockThinIndex cleared.\n");
    };
    
    CTxDB().Close();
    
    //fs::remove(GetPidFile());
    //return true;
	boost::filesystem::remove(GetPidFile());
    UnregisterAllWallets();
#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif
	globalVerifyHandle.reset();
    ECC_Stop();
	LogPrintf("Shutdown completed.\n\n");
}

/*void Shutdown()
{
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown) return;
    
    Finalise();
    
    LogPrintf("Shutdown complete.\n\n");
}*/

//
// Signal handlers are very limited in what they are allowed to do, so:
//
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//

bool static InitError(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, _("ProCurrency"), CClientUIInterface::BTN_OK | CClientUIInterface::MODAL);
    return false;
}

bool static InitWarning(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, _("ProCurrency"), CClientUIInterface::BTN_OK | CClientUIInterface::ICON_WARNING | CClientUIInterface::MODAL);
    return true;
}


bool static Bind(const CService &addr, bool fError = true)
{
    if (IsLimited(addr))
        return false;

    std::string strError;
    if (!BindListenPort(addr, strError))
    {
        if (fError)
            return InitError(strError);
        return false;
    };

    return true;
}

// Core-specific options shared between UI and daemon
std::string HelpMessage()
{
    std::string strUsage = _("Options:") + "\n";
    strUsage += "  -?                     " + _("This help message") + "\n";
    strUsage += "  -conf=<file>           " + _("Specify configuration file (default: proc.conf)") + "\n";
    strUsage += "  -pid=<file>            " + _("Specify pid file (default: procd.pid)") + "\n";
    strUsage += "  -datadir=<dir>         " + _("Specify data directory") + "\n";
    strUsage += "  -wallet=<dir>          " + _("Specify wallet file (within data directory)") + "\n";
    strUsage += "  -dbcache=<n>           " + _("Set database cache size in megabytes (default: 25)") + "\n";
    strUsage += "  -dblogsize=<n>         " + _("Set database disk log size in megabytes (default: 100)") + "\n";
    strUsage += "  -timeout=<n>           " + _("Specify connection timeout in milliseconds (default: 5000)") + "\n";
    strUsage += "  -proxy=<ip:port>       " + _("Connect through socks proxy") + "\n";
    strUsage += "  -socks=<n>             " + _("Select the version of socks proxy to use (4-5, default: 5)") + "\n";
    strUsage += "  -tor=<ip:port>         " + _("Use proxy to reach tor hidden services (default: same as -proxy)") + "\n";
    strUsage += "  -dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + "\n";
    strUsage += "  -port=<port>           " + _("Listen for connections on <port> (default: 22524 or testnet: 22525)") + "\n";
    strUsage += "  -maxconnections=<n>    " + _("Maintain at most <n> connections to peers (default: 125)") + "\n";
    strUsage += "  -addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n";
    strUsage += "  -connect=<ip>          " + _("Connect only to the specified node(s)") + "\n";
    strUsage += "  -seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n";
    strUsage += "  -externalip=<ip>       " + _("Specify your own public address") + "\n";
    strUsage += "  -onlynet=<net>         " + _("Only connect to nodes in network <net> (IPv4, IPv6 or Tor)") + "\n";
    strUsage += "  -discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n";
    strUsage += "  -listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n";
    strUsage += "  -bind=<addr>           " + _("Bind to given address. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -dnsseed               " + _("Find peers using DNS lookup (default: 1)") + "\n";
    strUsage += "  -staking               " + _("Stake your coins to support network and gain reward (default: 1)") + "\n";
    strUsage += "  -minstakeinterval=<n>  " + _("Minimum time in seconds between successful stakes (default: 30)") + "\n";
    strUsage += "  -minersleep=<n>        " + _("Milliseconds between stake attempts. Lowering this param will not result in more stakes. (default: 500)") + "\n";
    strUsage += "  -synctime              " + _("Sync time with other nodes. Disable if time on your system is precise e.g. syncing with NTP (default: 1)") + "\n";
    strUsage += "  -banscore=<n>          " + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n";
    strUsage += "  -bantime=<n>           " + _("Number of seconds to keep misbehaving peers from reconnecting (default: 86400)") + "\n";
    strUsage += "  -softbantime=<n>       " + _("Number of seconds to keep soft banned peers from reconnecting (default: 3600)") + "\n";
    strUsage += "  -maxreceivebuffer=<n>  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)") + "\n";
    strUsage += "  -maxsendbuffer=<n>     " + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)") + "\n";
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n";
#else
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 0)") + "\n";
#endif
#endif
    strUsage += "  -detachdb              " + _("Detach block and address databases. Increases shutdown time (default: 0)") + "\n";
    strUsage += "  -paytxfee=<amt>        " + _("Fee per KB to add to transactions you send") + "\n";
    strUsage += "  -mininput=<amt>        " + _("When creating transactions, ignore inputs with value less than this (default: 0.01)") + "\n";
    if (!fHaveGUI)
    {
        strUsage += "  -server                " + _("Accept command line and JSON-RPC commands") + "\n";
        strUsage += "  -cli                   " + _("Simple command line interface - if not run as daemon") + "\n";
    };

#if !defined(WIN32)
    if (!fHaveGUI)
        strUsage += "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
#endif
    strUsage += "  -testnet               " + _("Use the test network") + "\n";
    strUsage += "  -debug                 " + _("Output extra debugging information. Implies all other -debug* options") + "\n";
    strUsage += "  -debugnet              " + _("Output extra network debugging information") + "\n";
    strUsage += "  -debugchain            " + _("Output extra blockchain debugging information") + "\n";
    strUsage += "  -debugpos              " + _("Output extra Proof of Stake debugging information") + "\n";
    strUsage += "  -logtimestamps         " + _("Prepend debug output with timestamp") + "\n";
    strUsage += "  -shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n";
    strUsage += "  -printtoconsole        " + _("Send trace/debug info to console instead of debug.log file") + "\n";
    strUsage += "  -printtodebuglog       " + _("Send trace/debug info to debug.log file") + "\n";
    strUsage += "  -rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n";
    strUsage += "  -rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n";
    strUsage += "  -rpcport=<port>        " + _("Listen for JSON-RPC connections on <port> (default: 22523 or testnet: 22521)") + "\n";
    strUsage += "  -rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified IP address") + "\n";
    
    if (!fHaveGUI)
    {
        strUsage += "  -rpcconnect=<ip>       " + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n";
        strUsage += "  -rpcwait               " + _("Wait for RPC server to start") + "\n";
    };
    
    strUsage += "  -blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n";
    strUsage += "  -walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n";
    strUsage += "  -confchange            " + _("Require a confirmations for change (default: 0)") + "\n";
    strUsage += "  -enforcecanonical      " + _("Enforce transaction scripts to use canonical PUSH operators (default: 1)") + "\n";
    strUsage += "  -alertnotify=<cmd>     " + _("Execute command when a relevant alert is received (%s in cmd is replaced by message)") + "\n";
    strUsage += "  -upgradewallet         " + _("Upgrade wallet to latest format") + "\n";
    strUsage += "  -keypool=<n>           " + _("Set key pool size to <n> (default: 100)") + "\n";
    strUsage += "  -rescan                " + _("Rescan the block chain for missing wallet transactions") + "\n";
    strUsage += "  -salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + "\n";
    strUsage += "  -checkblocks=<n>       " + _("How many blocks to check at startup (default: 2500, 0 = all)") + "\n";
    strUsage += "  -checklevel=<n>        " + _("How thorough the block verification is (0-6, default: 1)") + "\n";
    strUsage += "  -loadblock=<file>      " + _("Imports blocks from external blk000?.dat file") + "\n";
    strUsage += "  -maxorphanblocksmib=<n> " + strprintf(_("Keep at most <n> MiB of unconnectable blocks in memory (default: %u)"), DEFAULT_MAX_ORPHAN_BLOCKS) + "\n";	
    strUsage += "  -reindex               " + _("Rebuild block chain index from current blk000?.dat files on startup") + "\n";
	/**** automatic backups ***/
	strUsage += "  -createwalletbackups=<n> " + _("Number of automatic wallet backups (default: 10)") + "\n";

    strUsage += "\n" + _("Thin options:") + "\n";
    strUsage += "  -thinmode              " + _("Operate in less secure, less resource hungry 'thin' mode") + "\n";
    strUsage += "  -thinfullindex         " + _("Keep the entire block index in memory. (default: 0)") + "\n";
    strUsage += "  -thinindexmax=<n>      " + _("When not thinfullindex, the max number of block headers to keep in memory. (default: 4096)") + "\n";

    strUsage += "  -nothinssupport        " + _("Disable supporting thin nodes. (default: 0)") + "\n";
    strUsage += "  -nothinstealth         " + _("Disable forwarding, or requesting all stealth txns. (default: 0)") + "\n";
    strUsage += "  -maxthinpeers=<n>      " + _("Don't connect to more than <n> thin peers (default: 8)") + "\n";

    strUsage += "\n" + _("Block creation options:") + "\n";
    strUsage += "  -blockminsize=<n>      "   + _("Set minimum block size in bytes (default: 0)") + "\n";
    strUsage += "  -blockmaxsize=<n>      "   + _("Set maximum block size in bytes (default: 250000)") + "\n";
    strUsage += "  -blockprioritysize=<n> "   + _("Set maximum size of high-priority/low-fee transactions in bytes (default: 27000)") + "\n";

    strUsage += "\n" + _("SSL options: (see the Bitcoin Wiki for SSL setup instructions)") + "\n";
    strUsage += "  -rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n";
    strUsage += "  -rpcsslcertificatechainfile=<file.cert>  " + _("Server certificate file (default: server.cert)") + "\n";
    strUsage += "  -rpcsslprivatekeyfile=<file.pem>         " + _("Server private key (default: server.pem)") + "\n";
    strUsage += "  -rpcsslciphers=<ciphers>                 " + _("Acceptable ciphers (default: TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH)") + "\n";

    strUsage += "\n" + _("Secure messaging options:") + "\n";
    strUsage += "  -nosmsg                                  " + _("Disable secure messaging.") + "\n";
    strUsage += "  -debugsmsg                               " + _("Log extra debug messages.") + "\n";
    strUsage += "  -smsgscanchain                           " + _("Scan the block chain for public key addresses on startup.") + "\n";
    
    return strUsage;
}

/** Sanity checks
 *  Ensure that Bitcoin is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if (!ECC_InitSanityCheck())
    {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.bitcoin.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    };

    // TODO: remaining sanity checks, see #4081

    return true;
}

/** Initialize bitcoin.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup)
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
// We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
// which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif
#ifndef WIN32
    umask(077);

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);
#endif

    if (!CheckDiskSpace())
        return false;

    // ********************************************************* Step 2: parameter interactions

    nNodeLifespan = GetArg("-addrlifespan", 7);

    nMinStakeInterval = GetArg("-minstakeinterval", 0);
    nMinerSleep = GetArg("-minersleep", 500);

    fUseFastIndex = GetBoolArg("-fastindex", true);

    // Largest block you're willing to create.
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    if (mapArgs.count("-mintxfee"))
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

    if (fDebug)
        LogPrintf("nMinerSleep %u\n", nMinerSleep);


    nDerivationMethodIndex = 0;

    fTestNet = GetBoolArg("-testnet", false);

    if (!SelectParamsFromCommandLine())
        return InitError("Invalid combination of -testnet and -regtest.");
    
    if (!fHaveGUI && GetBoolArg("-cli", false))
        printf("Network: %s\n", Params().NetworkIDString().c_str());
    
    if (GetBoolArg("-thinmode"))
        nNodeMode = NT_THIN;
    
    if (fTestNet)
    {
        SoftSetBoolArg("-irc", true);
    };

    if (mapArgs.count("-bind"))
    {
        // when specifying an explicit binding address, you want to listen on it
        // even when -connect or -proxy is specified
        SoftSetBoolArg("-listen", true);
    };

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        SoftSetBoolArg("-dnsseed", false);
        SoftSetBoolArg("-listen", false);
    }

    if (mapArgs.count("-proxy"))
    {
        // to protect privacy, do not listen by default if a proxy server is specified
        SoftSetBoolArg("-listen", false);
    };

    if (!GetBoolArg("-listen", true))
    {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        SoftSetBoolArg("-upnp", false);
        SoftSetBoolArg("-discover", false);
    };

    if (mapArgs.count("-externalip"))
    {
        // if an explicit public IP is specified, do not try to find others
        SoftSetBoolArg("-discover", false);
    };

    if (GetBoolArg("-salvagewallet"))
    {
        // Rewrite just private keys: rescan to find transactions
        SoftSetBoolArg("-rescan", true);
    };

    if (fTestNet)
    {
        nStakeMinAge = 1 * 60 * 60; // test net min age is 1 hour
        nCoinbaseMaturity = 10; // test maturity is 10 blocks
        nStakeMinConfirmations = 10; // test maturity is 10 blocks
    };

    // ********************************************************* Step 3: parameter-to-internal-flags
    
    fDebug = !mapMultiArgs["-debug"].empty();
    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const std::vector<std::string>& categories = mapMultiArgs["-debug"];
    if (GetBoolArg("-nodebug", false) || std::find(categories.begin(), categories.end(), std::string("0")) != categories.end())
        fDebug = false;
    
    // -debug implies fDebug*, unless otherwise specified
    if (fDebug)
    {
        SoftSetBoolArg("-debugnet", true);
        SoftSetBoolArg("-debugsmsg", true);
        SoftSetBoolArg("-debugchain", true);
        SoftSetBoolArg("-debugringsig", true);
    };

    fDebugNet = GetBoolArg("-debugnet");
    fDebugSmsg = GetBoolArg("-debugsmsg");
    fDebugChain = GetBoolArg("-debugchain");
    fDebugRingSig = GetBoolArg("-debugringsig");
    fDebugPoS = GetBoolArg("-debugpos");

    fNoSmsg = GetBoolArg("-nosmsg");
    
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("-socks"))
        return InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));

    bitdb.SetDetach(GetBoolArg("-detachdb", false));
    if (fDaemon)
        fServer = true;
    else
        fServer = GetBoolArg("-server", false);

    /* force fServer when running without GUI */
    if (!fHaveGUI)
        fServer = true;

    fPrintToConsole = GetBoolArg("-printtoconsole");
    fPrintToDebugLog = SoftSetBoolArg("-printtodebuglog", true);
    fLogTimestamps = GetBoolArg("-logtimestamps");
	
#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("-disablewallet", false);
#endif	

    if (mapArgs.count("-timeout"))
    {
        int nNewTimeout = GetArg("-timeout", 5000);
        if (nNewTimeout > 0 && nNewTimeout < 600000)
            nConnectTimeout = nNewTimeout;
    };

    if (mapArgs.count("-paytxfee"))
    {
        if (!ParseMoney(mapArgs["-paytxfee"], nTransactionFee))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"].c_str()));
        if (nTransactionFee > 0.25 * COIN)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
    };

    fConfChange = GetBoolArg("-confchange", false);
    fEnforceCanonical = GetBoolArg("-enforcecanonical", true);

    if (mapArgs.count("-mininput"))
    {
        if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue))
            return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"].c_str()));
    };


    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log
	
	// Initialize elliptic curve code
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());
	
    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. ProCurrency is shutting down."));

    
    std::string strDataDir = GetDataDir().string();
#ifdef ENABLE_WALLET	
    std::string strWalletFileName = GetArg("-wallet", "wallet.dat");

    // strWalletFileName must be a plain filename without a directory
    if (strWalletFileName != fs::basename(strWalletFileName) + fs::extension(strWalletFileName))
        return InitError(strprintf(_("Wallet %s resides outside data directory %s."), strWalletFileName.c_str(), strDataDir.c_str()));
#endif
    // Make sure only a single Bitcoin process is using the data directory.
    fs::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file)
        fclose(file);

    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
    if (!lock.try_lock())
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s.  ProCurrency is probably already running."), strDataDir.c_str()));
    
    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("ProCurrency version %s (%s)\n", FormatFullVersion().c_str(), CLIENT_DATE.c_str());
    LogPrintf("Operating in %s mode.\n", GetNodeModeName(nNodeMode));
    LogPrintf("Using OpenSSL version %s\n", SSLeay_version(SSLEAY_VERSION));

    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());

    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string().c_str());
    LogPrintf("Used data directory %s\n", strDataDir.c_str());

    std::ostringstream strErrors;

    if (fDaemon)
    {
        fprintf(stdout, "ProCurrency server starting\n");
        fflush(stdout);
    };
    
    int64_t nStart;



    /* *********************************************************
        Step 4.5: adjust parameters for nNodeMode
    ********************************************************* */

    switch (nNodeMode)
    {
        case NT_FULL:
            if (GetBoolArg("-nothinssupport"))
            {
                LogPrintf("Thin support disabled.\n");
                nLocalServices &= ~(THIN_SUPPORT);
            };

            if (GetBoolArg("-nothinstealth"))
            {
                LogPrintf("Thin stealth support disabled.\n");
                nLocalServices &= ~(THIN_STEALTH);
            };
            break;
        case NT_THIN:
            SetBoolArg("-staking", false);

            // -- clear services
            nLocalServices &= ~(NODE_NETWORK);
            nLocalServices &= ~(THIN_SUPPORT);
            nLocalServices &= ~(THIN_STAKE);
            nLocalServices &= ~(THIN_STEALTH);

            nLocalRequirements |= (THIN_SUPPORT);

            if (GetBoolArg("-thinfullindex"))
            {
                LogPrintf("Thin full index enabled.\n");
                fThinFullIndex = true;
            } else
            {
                nThinIndexWindow = GetArg("-thinindexmax", 4096);

                if (nThinIndexWindow < 4096)
                {
                    LogPrintf("Thin index window minimum size is %d.\n", 4096);
                    nThinIndexWindow = 4096;
                };

                LogPrintf("Thin index window size %d.\n", nThinIndexWindow);
            };

            if (GetBoolArg("-nothinstealth"))
            {
                LogPrintf("Thin stealth disabled.\n");
            } else
            {
                nLocalRequirements |= (THIN_STEALTH);
            };

            break;
        default:
            break;
    };

    // -- thin and full
    if (fNoSmsg)
        nLocalServices &= ~(SMSG_RELAY);

    if (initialiseRingSigs() != 0)
        return InitError("initialiseRingSigs() failed.");

    // ********************************************************* Step 5: backup wallet and verify database integrity
#ifdef ENABLE_WALLET
	if (!fDisableWallet) {

        filesystem::path backupDir = GetDataDir() / "backups";
        if (!filesystem::exists(backupDir))
        {
            // Always create backup folder to not confuse the operating system's file browser
            filesystem::create_directories(backupDir);
        }
        nWalletBackups = GetArg("-createwalletbackups", 10);
        nWalletBackups = std::max(0, std::min(10, nWalletBackups));
        if(nWalletBackups > 0)
        {
            if (filesystem::exists(backupDir))
            {
                // Create backup of the wallet
                std::string dateTimeStr = DateTimeStrFormat(".%Y-%m-%d-%H.%M", GetTime());
                std::string backupPathStr = backupDir.string();
                backupPathStr += "/" + strWalletFileName;
                std::string sourcePathStr = GetDataDir().string();
                sourcePathStr += "/" + strWalletFileName;
                boost::filesystem::path sourceFile = sourcePathStr;
                boost::filesystem::path backupFile = backupPathStr + dateTimeStr;
                sourceFile.make_preferred();
                backupFile.make_preferred();
                try {
                    boost::filesystem::copy_file(sourceFile, backupFile);
                    LogPrintf("Creating backup of %s -> %s\n", sourceFile, backupFile);
                } catch(boost::filesystem::filesystem_error &error) {
                    LogPrintf("Failed to create backup %s\n", error.what());
                }
                // Keep only the last 10 backups, including the new one of course
                typedef std::multimap<std::time_t, boost::filesystem::path> folder_set_t;
                folder_set_t folder_set;
                boost::filesystem::directory_iterator end_iter;
                boost::filesystem::path backupFolder = backupDir.string();
                backupFolder.make_preferred();
                // Build map of backup files for current(!) wallet sorted by last write time
                boost::filesystem::path currentFile;
                for (boost::filesystem::directory_iterator dir_iter(backupFolder); dir_iter != end_iter; ++dir_iter)
                {
                    // Only check regular files
                    if ( boost::filesystem::is_regular_file(dir_iter->status()))
                    {
                        currentFile = dir_iter->path().filename();
                        // Only add the backups for the current wallet, e.g. wallet.dat.*
                        if(currentFile.string().find(strWalletFileName) != string::npos)
                        {
                            folder_set.insert(folder_set_t::value_type(boost::filesystem::last_write_time(dir_iter->path()), *dir_iter));
                        }
                    }
                }
                // Loop backward through backup files and keep the N newest ones (1 <= N <= 10)
                int counter = 0;
                BOOST_REVERSE_FOREACH(PAIRTYPE(const std::time_t, boost::filesystem::path) file, folder_set)
                {
                    counter++;
                    if (counter > nWalletBackups)
                    {
                        // More than nWalletBackups backups: delete oldest one(s)
                        try {
                            boost::filesystem::remove(file.second);
                            LogPrintf("Old backup deleted: %s\n", file.second);
                        } catch(boost::filesystem::filesystem_error &error) {
                            LogPrintf("Failed to delete backup %s\n", error.what());
                        }
                    }
                }
            }
        }
	
		uiInterface.InitMessage(_("Verifying database integrity..."));

		if (!bitdb.Open(GetDataDir()))
		{
			std::string msg = strprintf(_("Error initializing database environment %s!"
				" To recover, BACKUP THAT DIRECTORY, then remove"
				" everything from it except for wallet.dat."), strDataDir.c_str());
			return InitError(msg);
		}

		if (GetBoolArg("-salvagewallet"))
		{
			// Recover readable keypairs:
			if (!CWalletDB::Recover(bitdb, strWalletFileName, true))
				return false;
		}

		if (fs::exists(GetDataDir() / strWalletFileName))
		{
			CDBEnv::VerifyResult r = bitdb.Verify(strWalletFileName, CWalletDB::Recover);
			if (r == CDBEnv::RECOVER_OK)
			{
				std::string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
					" Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
					" your balance or transactions are incorrect you should"
					" restore from a backup."), strDataDir.c_str());
				uiInterface.ThreadSafeMessageBox(msg, _("ProCurrency"), CClientUIInterface::BTN_OK | CClientUIInterface::ICON_WARNING | CClientUIInterface::MODAL);
			}

			if (r == CDBEnv::RECOVER_FAIL)
				return InitError(_("wallet.dat corrupt, salvage failed"));
		}
	} // (!fDisableWallet)
#endif // ENABLE_WALLET
    // ********************************************************* Step 6: network initialization

    nMaxThinPeers = GetArg("-maxthinpeers", 8);

    nBloomFilterElements = GetArg("-bloomfilterelements", 1536);
	
	RegisterNodeSignals(GetNodeSignals());

    // format user agent, check total size
    strSubVersion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, mapMultiArgs.count("-uacomment") ? mapMultiArgs["-uacomment"] : std::vector<string>());
    if (strSubVersion.size() > MAX_SUBVERSION_LENGTH) {
        return InitError(strprintf("Total length of network version string %i exceeds maximum of %i characters. Reduce the number and/or size of uacomments.",
            strSubVersion.size(), MAX_SUBVERSION_LENGTH));
    }
    

    if (mapArgs.count("-onlynet"))
    {
        std::set<enum Network> nets;
        BOOST_FOREACH(std::string snet, mapMultiArgs["-onlynet"])
        {
            enum Network net = ParseNetwork(snet);
            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet.c_str()));
            nets.insert(net);
        };
        for (int n = 0; n < NET_MAX; n++)
        {
            enum Network net = (enum Network)n;
            if (!nets.count(net))
                SetLimited(net);
        };
    };

    CService addrProxy;
    bool fProxy = false;
    if (mapArgs.count("-proxy")) {
        addrProxy = CService(mapArgs["-proxy"], 9050);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["-proxy"]));
		
        if (!IsLimited(NET_IPV4))
			SetProxy(NET_IPV4, addrProxy);
		if (!IsLimited(NET_IPV6))
			SetProxy(NET_IPV6, addrProxy);
        SetNameProxy(addrProxy);
        fProxy = true;
    }

    // -tor can override normal proxy, -notor disables tor entirely
    if (!(mapArgs.count("-tor") && mapArgs["-tor"] == "0") 
        && (fProxy || mapArgs.count("-tor")))
    {
        CService addrOnion;
        if (!mapArgs.count("-tor"))
            addrOnion = addrProxy;
        else
            addrOnion = CService(mapArgs["-tor"], 9050);
        if (!addrOnion.IsValid())
            return InitError(strprintf(_("Invalid -tor address: '%s'"), mapArgs["-tor"]));
        SetProxy(NET_TOR, addrOnion);
        SetReachable(NET_TOR);
    }

    // see Step 2: parameter interactions for more information about these
    fNoListen = !GetBoolArg("-listen", true);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);
#ifdef USE_UPNP
    fUseUPnP = GetBoolArg("-upnp", USE_UPNP);
#endif

    bool fBound = false;
    if (!fNoListen)
    {
        std::string strError;
        if (mapArgs.count("-bind"))
        {
            BOOST_FOREACH(std::string strBind, mapMultiArgs["-bind"])
            {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind.c_str()));
                fBound |= Bind(addrBind);
            };
        } else {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            if (!IsLimited(NET_IPV6))
                fBound |= Bind(CService(in6addr_any, GetListenPort()), false);
            if (!IsLimited(NET_IPV4))
                fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound);
        };
        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    };

    if (mapArgs.count("-externalip"))
    {
        BOOST_FOREACH(std::string strAddr, mapMultiArgs["-externalip"])
        {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);
            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr.c_str()));
            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        };
    };

#ifdef ENABLE_WALLET	
    if (mapArgs.count("-reservebalance")) // procurrency: reserve balance amount
    {
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        {
            InitError(_("Invalid amount for -reservebalance=<amount>"));
            return false;
        }
    }
#endif
   
    BOOST_FOREACH(std::string strDest, mapMultiArgs["-seednode"])
        AddOneShot(strDest);

    // ********************************************************* Step 7: load blockchain

    if (!bitdb.Open(GetDataDir()))
    {
        std::string msg = strprintf(_("Error initializing database environment %s!"
                                 " To recover, BACKUP THAT DIRECTORY, then remove"
                                 " everything from it except for wallet.dat."), strDataDir.c_str());
        return InitError(msg);
    };

    if (GetBoolArg("-loadblockindextest"))
    {
        CTxDB txdb("r");
        txdb.LoadBlockIndex();
        PrintBlockTree();
        return false;
    };

    uiInterface.InitMessage(_("Loading block index..."));
    LogPrintf("Loading block index...\n");
    nStart = GetTimeMillis();
    
    // -- wipe the txdb if a reindex is queued
    if (mapArgs.count("-reindex"))
    {
        LOCK(cs_main);
        CTxDB txdb("cr+");
        txdb.RecreateDB();
    };
    
    switch (LoadBlockIndex())
    {
        case 1:
            return InitError(_("Error loading blkindex.dat"));
        case 2:
            if (nNodeMode == NT_FULL)
            {
                LogPrintf("TxDb wiped, reindex requested.\n");
                mapArgs["-reindex"] = 1;
            };
            break;
    };
    
    // as LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill bitcoin-qt during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    };

    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    if (GetBoolArg("-printblockindex") || GetBoolArg("-printblocktree"))
    {
        PrintBlockTree();
        return false;
    };

    if (mapArgs.count("-printblock"))
    {
        std::string strMatch = mapArgs["-printblock"];
        int nFound = 0;
        for (std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
        {
            uint256 hash = (*mi).first;
            if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
            {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                block.ReadFromDisk(pindex);
                block.BuildMerkleTree();
                block.print();
                LogPrintf("\n");
                nFound++;
            };
        };
        if (nFound == 0)
            LogPrintf("No blocks matching %s were found\n", strMatch.c_str());
        return false;
    };

    // ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
    if (fDisableWallet) {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    } else {
		uiInterface.InitMessage(_("Loading wallet..."));
		LogPrintf("Loading wallet...\n");
		nStart = GetTimeMillis();
    
		pwalletMain = new CWallet(strWalletFileName);
		DBErrors nLoadWalletRet = pwalletMain->LoadWallet();

		if (nLoadWalletRet != DB_LOAD_OK)
		{
			if (nLoadWalletRet == DB_CORRUPT)
			{
				strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
			} else
			if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
			{
				std::string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                         " or address book entries might be missing or incorrect."));
				uiInterface.ThreadSafeMessageBox(msg, _("ProCurrency"), CClientUIInterface::BTN_OK | CClientUIInterface::ICON_WARNING | CClientUIInterface::MODAL);
			} else
			if (nLoadWalletRet == DB_TOO_NEW)
			{
				strErrors << _("Error loading wallet.dat: Wallet requires newer version of ProCurrency") << "\n";
			} else
			if (nLoadWalletRet == DB_NEED_REWRITE)
			{
				strErrors << _("Wallet needed to be rewritten: restart ProCurrency to complete") << "\n";
				LogPrintf("%s", strErrors.str().c_str());
				return InitError(strErrors.str());
			} else
			{
				strErrors << _("Error loading wallet.dat") << "\n";
			}
		}
    
		// --- Prepare extended keys
		pwalletMain->ExtKeyLoadMaster();
		pwalletMain->ExtKeyLoadAccounts();
		pwalletMain->ExtKeyLoadAccountPacks();
    
    
    
		LogPrintf("%s", strErrors.str().c_str());
		LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);


		RegisterWallet(pwalletMain);

		CBlockIndex *pindexRescan = pindexBest;
		if (GetBoolArg("-rescan"))
		{
			pindexRescan = pindexGenesisBlock;
		} else
		{
			CWalletDB walletdb(strWalletFileName);
			CBlockLocator locator;
			if (walletdb.ReadBestBlock(locator))
				pindexRescan = locator.GetBlockIndex();
		}

		if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight)
		{
			uiInterface.InitMessage(_("Rescanning..."));
			LogPrintf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
			nStart = GetTimeMillis();
			pwalletMain->ScanForWalletTransactions(pindexRescan, true);
			LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
		}
	} // (!fDisableWallet)
#else // ENABLE_WALLET
    LogPrintf("No wallet compiled in!\n");
#endif // !ENABLE_WALLET	
    // ********************************************************* Step 9: import blocks

    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("-loadblock"))
    {
        BOOST_FOREACH(string strFile, mapMultiArgs["-loadblock"])
            vImportFiles.push_back(strFile);
    }
    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));
    
    // ********************************************************* Step 10: load peers

    uiInterface.InitMessage(_("Loading addresses..."));
    LogPrintf("Loading addresses...\n");
    nStart = GetTimeMillis();

    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
        addrman.size(), GetTimeMillis() - nStart);


    // ********************************************************* Step 10.1: startup secure messaging

    SecureMsgStart(fNoSmsg, GetBoolArg("-smsgscanchain"));

    // ********************************************************* Step 11: start node

    if (!CheckDiskSpace())
        return false;

    RandAddSeedPerfmon();
	
	/* // TO DO: RebuildAddressIndex
	// reindex addresses found in blockchain
    if(GetBoolArg("-reindexaddr", false))
    {
        uiInterface.InitMessage(_("Rebuilding address index..."));
        CBlockIndex *pblockAddrIndex = pindexBest;
		CTxDB txdbAddr("rw");
		while(pblockAddrIndex)
		{
			uiInterface.InitMessage(strprintf("Rebuilding address index, block %i", pblockAddrIndex->nHeight));
			bool ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions=true);
			CBlock pblockAddr;
			if(pblockAddr.ReadFromDisk(pblockAddrIndex, true))
				pblockAddr.RebuildAddressIndex(txdbAddr);
			pblockAddrIndex = pblockAddrIndex->pprev;
		}
    }
	*/

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n",            mapBlockIndex.size());
    LogPrintf("mapBlockThinIndex.size() = %u\n",        mapBlockThinIndex.size());
    LogPrintf("nBestHeight = %d\n",                     nBestHeight);
#ifdef ENABLE_WALLET
    LogPrintf("setKeyPool.size() = %u\n",      pwalletMain ? pwalletMain->setKeyPool.size() : 0);
    LogPrintf("mapWallet.size() = %u\n",       pwalletMain ? pwalletMain->mapWallet.size() : 0);
    LogPrintf("mapAddressBook.size() = %u\n",  pwalletMain ? pwalletMain->mapAddressBook.size() : 0);
#endif

    StartNode(threadGroup);
    
    if (fServer)
        StartRPCThreads();

    // ********************************************************* Step 12: finished
    
    // Add wallet transactions that aren't already in a block to mapTransactions
    pwalletMain->ReacceptWalletTransactions();
    
    // Run a thread to flush wallet periodically
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)(const std::string&), const std::string&>, "wflush", &ThreadFlushWalletDB, boost::ref(pwalletMain->strWalletFile)));
    
    InitRPCMining();
    
    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("-staking", true))
        LogPrintf("Staking disabled\n");
    else
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)(CWallet*), CWallet*>, "miner", &ThreadStakeMiner, pwalletMain));
    
    if (nNodeMode != NT_FULL)
        pwalletMain->InitBloomFilter();

    uiInterface.InitMessage(_("Done loading"));
    LogPrintf("Done loading.\n");

    if (!strErrors.str().empty())
        return InitError(strErrors.str());
    
    LogPrintf("Network: %s, port: %d\n", Params().NetworkIDString(), Params().GetDefaultPort());
    
    return !fRequestShutdown;
}
