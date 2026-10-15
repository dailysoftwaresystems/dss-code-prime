// THE ONE REDACTOR'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry `harness/redact_selftest`.
//
// WHAT THIS IS. `.harness-config/runner/actions/redact/redact.py` is the one statement of what must never
// leave a machine — tree paths, homes, accounts, host names, addresses, secret paths — which the
// repository's programs load by path and a session pipes its output through. Run with `--self-test` it
// proves every rule and both exemptions in both directions, against an exact arm count, and prints one
// verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
//
// ★ IT IS THE ENTRY, NOT A SECOND ONE. Until 2026-10-15 the program was started bare by ctest: one
// verdict for every arm, so no mutation of it could be an arm of the mutation sweep. Only its
// self-test's lines changed form: the filter's own output did not.
// ⓘ No `repo-guard` label, as before: what must be hidden depends on the HOST's own account and machine
// names, so every leg runs it.

#include "dss_redact_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_REDACT_SELFTEST_ARMS(X)                                                                                  \
    X(NulBytesAreDropped, "NUL bytes are dropped")                                                                     \
    X(TheTreePathIsTree, "the tree path is <tree>")                                                                    \
    X(TheHomePathIs, "the home path is ~")                                                                             \
    X(AWindowsHomeAnySlashAnyCaseIs, "a Windows home, any slash, any case, is ~")                                      \
    X(SecretPathKeyAtSrvKSshIdEd25519Ok, "secret path 'key at /srv/k/.ssh/id_ed25519 ok'")                             \
    X(SecretPathReadHarnessConfigSshItemsVps7EnvNow, "secret path 'read .harness-config/sshItems/vps7/.env now'")      \
    X(SecretPathCKeysVps7Pem, "secret path 'C:\\\\keys\\\\vps7.pem'")                                                  \
    X(SecretPathCert7Pem, "secret path 'cert7.pem.'")                                                                  \
    X(SecretPathOptKeysDeploy7Key, "secret path '/opt/keys/deploy7.key'")                                              \
    X(SecretPathLogsIdCustom7, "secret path 'logs/id_custom7'")                                                        \
    X(SecretPathFoundIdEd25519Vps7PubHere, "secret path 'found id_ed25519_vps7.pub here'")                             \
    X(SecretPathTokInSecretsToken7Txt, "secret path 'tok in .secrets/token7.txt'")                                     \
    X(SecretPathCfgEnvLocal, "secret path 'cfg .env.local'")                                                           \
    X(SecretPathIdentityFileSrvKSshK7, "secret path 'IdentityFile=/srv/k/.ssh/k7'")                                    \
    X(SecretPathISrvKSshK7, "secret path '-i/srv/k/.ssh/k7'")                                                          \
    X(NotASecretPathSelfKey1, "not a secret path: 'self.key = 1'")                                                     \
    X(NotASecretPathTheIdCounterField, "not a secret path: 'the id_counter field'")                                    \
    X(NotASecretPathXEnvironment, "not a secret path: 'x.environment'")                                                \
    X(NotASecretPathMyEnvyX, "not a secret path: 'my.envy/x'")                                                         \
    X(HomeShapeCUsersOtheracctAppDataLocalTempX,                                                                       \
      "home shape 'C:\\\\Users\\\\otheracct\\\\AppData\\\\Local\\\\Temp\\\\x'")                                        \
    X(HomeShapePathCUsersOtheracctSrc, "home shape 'path C:\\\\\\\\Users\\\\\\\\otheracct\\\\\\\\src'")                \
    X(HomeShapeCUsersOtheracctX, "home shape 'c:/users/otheracct/x'")                                                  \
    X(HomeShapeDUsersAnnLeeAppData, "home shape 'D:\\\\Users\\\\Ann Lee\\\\AppData'")                                  \
    X(HomeShapeMntCUsersOtheracctAppData, "home shape '/mnt/c/Users/otheracct/AppData'")                               \
    X(HomeShapeMntDUsersAnnLeeX, "home shape '/mnt/d/users/Ann Lee/x'")                                                \
    X(HomeShapeCdCUsersOtheracctSrc, "home shape 'cd /c/Users/otheracct/src'")                                         \
    X(HomeShapeUsersMacacctLibraryX, "home shape '/Users/macacct/Library/x'")                                          \
    X(HomeShapeCdHomeOtheracctSrc, "home shape 'cd /home/otheracct/src'")                                              \
    X(HomeShapeHomeHomeAb, "home shape 'HOME=/home/ab'")                                                               \
    X(HomeShapeGccIHomeOtheracctInclude, "home shape 'gcc -I/home/otheracct/include'")                                 \
    X(HomeShapeHomeOtheracct, "home shape \"'/home/otheracct'\"")                                                      \
    X(HomeShapeHomeAnnLee, "home shape '\"/home/Ann Lee\"'")                                                           \
    X(HomeShapeWslLocalhostUbuntuHomeOtheracctP44, "home shape '//wsl.localhost/Ubuntu/home/otheracct/p44'")           \
    X(HomeShapeWslLocalhostUbuntuHomeOtheracctX,                                                                       \
      "home shape '\\\\\\\\wsl.localhost\\\\Ubuntu\\\\home\\\\otheracct\\\\x'")                                        \
    X(HomeShapeWslUbuntuHomeOtheracct, "home shape '\\\\\\\\wsl$\\\\Ubuntu\\\\home\\\\otheracct'")                     \
    X(HomeShapeZHomeOtheracctTest, "home shape 'Z:\\\\home\\\\otheracct\\\\test'")                                     \
    X(HomeShapeZHomeOtheracctSrc, "home shape 'Z:/home/otheracct/src'")                                                \
    X(HomeShapeCygdriveCUsersOtheracctX, "home shape '/cygdrive/c/Users/otheracct/x'")                                 \
    X(HomeShapeFileserverHomeOtheracctDocs, "home shape '\\\\\\\\fileserver\\\\home\\\\otheracct\\\\docs'")            \
    X(HomeShapeFileHomeOtheracctAC, "home shape 'file:///home/otheracct/a.c'")                                         \
    X(HomeShapeHomeOtheracctX, "home shape '/HOME/Otheracct/x'")                                                       \
    X(HomeShapeCUsersOBrienX, "home shape \"C:\\\\Users\\\\O'Brien\\\\x\"")                                            \
    X(HomeShapeCUsersOBrien, "home shape \"C:\\\\Users\\\\O'Brien\"")                                                  \
    X(HomeShapeDocsHomeGuideMd, "home shape 'docs/home/guide.md'")                                                     \
    X(HomeShapeFilesInHomeOtheracctAndMoreWordsHere, "home shape 'files in /home/otheracct and more words here'")      \
    X(HomeShapeCdHomeOtheracctAndDonTTouchSrcX, "home shape \"cd /home/otheracct and don't touch src/x\"")             \
    X(HomeShapeCUsersOtheracctAndDonTTouchSrcX, "home shape \"C:\\\\Users\\\\otheracct and don't touch src\\\\x\"")    \
    X(HomeShapeCUsersOtheracctAndManyMoreWordsX, "home shape 'C:\\\\Users\\\\otheracct and many more words\\\\x'")     \
    X(HomeShapeEchoHomeOtheracctAndFix, "home shape 'echo /home/otheracct and fix\"'")                                 \
    X(HomeShapeCUsersAnnLeeCorpX, "home shape 'C:\\\\Users\\\\Ann Lee.CORP\\\\x'")                                     \
    X(HomeShapeHomeAnnLee2, "home shape \"'/home/Ann Lee'\"")                                                          \
    X(NotAHomeTheUsersGuide, "not a home: 'the Users guide'")                                                          \
    X(NotAHomeHomeX, "not a home: 'home/x'")                                                                           \
    X(NotAHomeHomeworkX, "not a home: '/homework/x'")                                                                  \
    X(NotAHomeUsersGuideX, "not a home: '/users-guide/x'")                                                             \
    X(NotAHomeMyhomeX, "not a home: '/myhome/x'")                                                                      \
    X(NotAHomeSrcACpp, "not a home: '~/src/a.cpp'")                                                                    \
    X(NotAHomeHomeUserSrc, "not a home: '/home/<user>/src'")                                                           \
    X(NameIsAStrangerSHome, "~name/ is a stranger's home")                                                             \
    X(NameAloneIsAStrangerSHome, "~name alone is a stranger's home")                                                   \
    X(IsNotAStrangerSHome, "~/ is not a stranger's home")                                                              \
    X(An83ShortNameIsNotAHome, "an 8.3 short name is not a home")                                                      \
    X(UserHostOfAnUnknownAccount, "user@host of an unknown account")                                                   \
    X(UserHostInCapitals, "USER@HOST in capitals")                                                                     \
    X(AnAssemblerSuffixIsKeptPutsPlt, "an assembler suffix is kept: puts@plt")                                         \
    X(AnAssemblerSuffixIsKeptXGotpcrelRip, "an assembler suffix is kept: x@GOTPCREL(%rip)")                            \
    X(AnAssemblerSuffixIsKeptGPageoff, "an assembler suffix is kept: _g@PAGEOFF")                                      \
    X(AnAssemblerSuffixIsKeptVTlsgd, "an assembler suffix is kept: v@tlsgd")                                           \
    X(AnAssemblerSuffixIsKeptFSecrel32, "an assembler suffix is kept: f@SECREL32")                                     \
    X(AHostBeginningLikeASuffixBob7GothamExample, "a host beginning like a suffix: bob7@gotham.example")               \
    X(AHostBeginningLikeASuffixAnn7Page7Example, "a host beginning like a suffix: ann7@page7.example")                 \
    X(AHostBeginningLikeASuffixEve7AbsentExample, "a host beginning like a suffix: eve7@absent.example")               \
    X(AHostBeginningLikeASuffixJoe7GotExample, "a host beginning like a suffix: joe7@got.example")                     \
    X(AHostBeginningLikeASuffixKim7PltHostExample, "a host beginning like a suffix: kim7@plt-host.example")            \
    X(AHostBeginningLikeASuffixPat78Example, "a host beginning like a suffix: pat7@8.example")                         \
    X(UserIpv6, "user@[ipv6]")                                                                                         \
    X(AVersionIsKeptBrewInstallTclTk8Llvm17Python312,                                                                  \
      "a version is kept: 'brew install tcl-tk@8 llvm@17 python@3.12'")                                                \
    X(AVersionIsKeptUsesActionsCheckoutV4, "a version is kept: 'uses: actions/checkout@v4'")                           \
    X(AnMDNSNameIsMasked, "an mDNS name is masked")                                                                    \
    X(AnMDNSNameInCapitalsIsMasked, "an mDNS name in capitals is masked")                                              \
    X(NotAnMDNSNameTheHostsLocalSection, "not an mDNS name: 'the hosts.local section'")                                \
    X(NotAnMDNSNameEditClaudeSettingsLocalJson, "not an mDNS name: 'edit .claude/settings.local.json'")                \
    X(SshCommandSshIOptKK1Vps7Uptime, "ssh command 'ssh -i /opt/k/k1 vps7 uptime'")                                    \
    X(SshCommandSshIOptKK1Vps7, "ssh command 'ssh -i/opt/k/k1 vps7'")                                                  \
    X(SshCommandSshLRemoteacctVps7, "ssh command 'ssh -l remoteacct vps7'")                                            \
    X(SshCommandSshLremoteacctVps7, "ssh command 'ssh -lremoteacct vps7'")                                             \
    X(SshCommandSshFOptCfgC7Vps7, "ssh command 'ssh -F /opt/cfg/c7 vps7'")                                             \
    X(SshCommandSshJJump7Vps7, "ssh command 'ssh -J jump7 vps7'")                                                      \
    X(SshCommandSshOUserRemoteacctOHostNameVps7ExampleOIdentityFileOptK1OProxyJumpJump7Alias7,                         \
      "ssh command 'ssh -o User=remoteacct -o HostName=vps7.example -o IdentityFile=/opt/k1 -o ProxyJump=jump7 alias7'") \
    X(SshCommandSshOUserRemoteacctAlias7, "ssh command 'ssh -oUser=remoteacct alias7'")                                \
    X(SshCommandSshOUserRemoteacctAlias72, "ssh command 'ssh -o \"User remoteacct\" alias7'")                          \
    X(SshCommandSshOProxyCommandNc7Alias7, "ssh command 'ssh -o ProxyCommand=nc7 alias7'")                             \
    X(SshCommandSshOBatchModeYesOConnectTimeout25Alias7True,                                                           \
      "ssh command 'ssh -o BatchMode=yes -o ConnectTimeout=25 alias7 true'")                                           \
    X(SshCommandSshVttP22L8080Inner780Alias7, "ssh command 'ssh -vtt -p 22 -L 8080:inner7:80 alias7'")                 \
    X(SshCommandSshWInner722Jump7, "ssh command 'ssh -W inner7:22 jump7'")                                             \
    X(SshCommandSshSshRemoteacctVps72222, "ssh command 'ssh ssh://remoteacct@vps7:2222'")                              \
    X(SshCommandCWindowsSystem32OpenSSHSshExeLRemoteacctAlias7Uname,                                                   \
      "ssh command 'C:\\\\Windows\\\\System32\\\\OpenSSH\\\\ssh.exe -l remoteacct alias7 uname'")                      \
    X(SshCommandScpIOptK1P22L800NotesTxtRemoteacctVps7SrvX,                                                            \
      "ssh command 'scp -i /opt/k1 -P 22 -l 800 notes.txt remoteacct@vps7:/srv/x'")                                    \
    X(SshCommandScpVps7SrvACLocalB, "ssh command 'scp vps7:/srv/a C:\\\\local\\\\b'")                                  \
    X(SshCommandRsyncAvzESshIOptK1LRemoteacctSrcVps7Dst,                                                               \
      "ssh command 'rsync -avz -e \"ssh -i /opt/k1 -l remoteacct\" src/ vps7:dst/'")                                   \
    X(SshCommandRsyncRshSshIOptK1SrcVps7Mod7, "ssh command 'rsync --rsh=\"ssh -i /opt/k1\" src/ vps7::mod7'")          \
    X(SshCommandRsyncAvRsyncVps7Mod7Dst, "ssh command 'rsync -av rsync://vps7/mod7 dst/'")                             \
    X(SshCommandSftpFOptCfgC7Vps7, "ssh command 'sftp -F /opt/cfg/c7 vps7'")                                           \
    X(SshCommandMoshVps7, "ssh command 'mosh vps7'")                                                                   \
    X(SshCommandSshJump7SshLRemoteacctInner7Uptime, "ssh command 'ssh jump7 ssh -l remoteacct inner7 uptime'")         \
    X(SshCommandXSshVps7LsSshIOptK1Vps8, "ssh command 'x && ssh vps7 ls; ssh -i /opt/k1 vps8'")                        \
    X(SshCommandGitSshCommandSshLRemoteacctIOptK1GitFetch,                                                             \
      "ssh command 'GIT_SSH_COMMAND=\"ssh -l remoteacct -i /opt/k1\" git fetch'")                                      \
    X(SshCommandExportRsyncRshSshIOptK1LRemoteacct,                                                                    \
      "ssh command \"export RSYNC_RSH='ssh -i /opt/k1 -l remoteacct'\"")                                               \
    X(SshCommandRsyncESshLRemoteacctSrcVps7Dst, "ssh command \"rsync -e'ssh -l remoteacct' src/ vps7:dst/\"")          \
    X(ProseIsNotAHostCopiedOverSshAndVerified, "prose is not a host: 'copied over ssh and verified'")                  \
    X(ProseIsNotAHostConnectingToTheDatabase, "prose is not a host: 'Connecting to the database'")                     \
    X(ProseIsNotAHostConnectedToTheNetwork, "prose is not a host: 'Connected to the network.'")                        \
    X(ADeclaredItemSNameIsKeptSshItem7, "a declared item's name is kept: ssh item7:")                                  \
    X(NotAnSshCommandSshConnectToNothing, "not an ssh command: 'ssh: connect to nothing'")                             \
    X(NotAnSshCommandRsyncErrorSomeFilesWereNotTransferred,                                                            \
      "not an ssh command: 'rsync error: some files were not transferred'")                                            \
    X(NotAnSshCommandTheSshConfigFile, "not an ssh command: 'the ssh_config file'")                                    \
    X(NotAnSshCommandOpensshClient, "not an ssh command: 'openssh-client'")                                            \
    X(NotAnSshCommandSshIsGone, "not an ssh command: '~/.ssh is gone'")                                                \
    X(SshMessageSshConnectToHostVps7ExampleNetPo, "ssh message 'ssh: connect to host vps7.example.net po'")            \
    X(SshMessageSshConnectToHost2001Db87Port22, "ssh message 'ssh: connect to host 2001:db8::7 port 22'")              \
    X(SshMessageSshConnectToHostFe80123456789ab, "ssh message 'ssh: connect to host fe80::1234:5678:9ab'")             \
    X(SshMessageSshConnectToAddress2001Db80000, "ssh message 'ssh: connect to address 2001:db8:0:0:0:0'")              \
    X(SshMessageSshCouldNotResolveHostnameVps7Na, "ssh message 'ssh: Could not resolve hostname vps7: Na'")            \
    X(SshMessageConnectionToVps7ExampleNetClosed, "ssh message 'Connection to vps7.example.net closed.'")              \
    X(SshMessageConnectionClosedBy10009Port22, "ssh message 'Connection closed by 10.0.0.9 port 22'")                  \
    X(SshMessageConnectionClosedByAuthenticatingUser, "ssh message 'Connection closed by authenticating user'")        \
    X(SshMessageWarningPermanentlyAddedVps7Example, "ssh message \"Warning: Permanently added 'vps7.example\"")        \
    X(SshMessageTheAuthenticityOfHostVps710009, "ssh message \"The authenticity of host 'vps7 (10.0.0.9\"")            \
    X(SshMessageHostKeyForVps7HasChangedAndYouHa, "ssh message 'Host key for vps7 has changed and you ha'")            \
    X(SshMessageAuthenticatedToVps71000922Us, "ssh message 'Authenticated to vps7 ([10.0.0.9]:22) us'")                \
    X(SshMessageConnectedToVps7, "ssh message 'Connected to vps7.'")                                                   \
    X(SshMessageConnectingToVps7, "ssh message 'Connecting to vps7...'")                                               \
    X(AnIPv4AddressIsMasked, "an IPv4 address is masked")                                                              \
    X(AnIPv4AddressJoinedByIsMasked, "an IPv4 address joined by _ is masked")                                          \
    X(ACSectionIsKeptC6725SRule, "a C section is kept: \"C 6.7.2.5's rule\"")                                          \
    X(ACSectionIsKeptC236725, "a C section is kept: 'C23 6.7.2.5'")                                                    \
    X(ACSectionIsKept6725, "a C section is kept: '\xC2\xA7" "6.7.2.5'")                                                \
    X(ACSectionIsKept67252, "a C section is kept: '\xC2\xA7 6.7.2.5'")                                                 \
    X(ACSectionIsKeptSection6725, "a C section is kept: 'section 6.7.2.5'")                                            \
    X(ACSectionIsKept6725p3, "a C section is kept: '6.7.2.5p3'")                                                       \
    X(AnIPv6AddressIsMaskedAddrFe801Eth0Up, "an IPv6 address is masked: 'addr fe80::1%eth0 up'")                       \
    X(AnIPv6AddressIsMasked2001Db8722, "an IPv6 address is masked: '[2001:db8::7]:22'")                                \
    X(AnIPv6AddressIsMasked20010db8000000000000Ff0000428329,                                                           \
      "an IPv6 address is masked: '2001:0db8:0000:0000:0000:ff00:0042:8329'")                                          \
    X(AnIPv6AddressIsMaskedLo1, "an IPv6 address is masked: 'lo ::1'")                                                 \
    X(AnIPv6AddressIsMaskedFfff20301135, "an IPv6 address is masked: '::ffff:203.0.113.5'")                            \
    X(AnIPv6AddressIsMaskedNat6464Ff9b1920233Up, "an IPv6 address is masked: 'nat64 64:ff9b::192.0.2.33 up'")          \
    X(AnIPv6AddressIsMaskedFfff2030113522, "an IPv6 address is masked: '[::ffff:203.0.113.5]:22'")                     \
    X(AnIPv6AddressIsMaskedFfff20301135222, "an IPv6 address is masked: '::ffff:203.0.113.5:22'")                      \
    X(NotAnIPv6AddressStdVectorInt, "not an IPv6 address: 'std::vector<int>'")                                         \
    X(NotAnIPv6AddressDssTree, "not an IPv6 address: 'dss::Tree'")                                                     \
    X(NotAnIPv6AddressAB, "not an IPv6 address: 'a::b'")                                                               \
    X(NotAnIPv6AddressAt123456, "not an IPv6 address: 'at 12:34:56'")                                                  \
    X(NotAnIPv6AddressCafeBeef, "not an IPv6 address: 'cafe::beef'")                                                   \
    X(LsLOwnerAndGroup, "ls -l: owner and group")                                                                      \
    X(LsLMacOSOwnerAndGroup, "ls -l (macOS @): owner and group")                                                       \
    X(NotAnLsLLine, "not an ls -l line")                                                                               \
    X(AUncServerIsMaskedFileserver7ShareX, "a UNC server is masked: '\\\\\\\\fileserver7\\\\share\\\\x'")              \
    X(AUncServerIsMaskedFileserver7Share, "a UNC server is masked: '//fileserver7/share'")                             \
    X(AUncServerIsMaskedNetUseNas7Pub, "a UNC server is masked: 'net use \\\\\\\\nas7\\\\pub'")                        \
    X(NotAUncServerHttpsWwwExampleOrgDownloadHtml, "not a UNC server: 'https://www.example.org/download.html'")        \
    X(NotAUncServerWslLocalhostUbuntuX, "not a UNC server: '\\\\\\\\wsl.localhost\\\\Ubuntu\\\\x'")                    \
    X(NotAUncServerWslUbuntuX, "not a UNC server: '\\\\\\\\wsl$\\\\Ubuntu\\\\x'")                                      \
    X(NotAUncServerCX, "not a UNC server: '\\\\\\\\?\\\\C:\\\\x'")                                                     \
    X(NotAUncServerABC, "not a UNC server: 'a//b/c'")                                                                  \
    X(NotAUncServerLocalhostCX, "not a UNC server: '\\\\\\\\localhost\\\\c$\\\\x'")                                    \
    X(ANameInsideAVersionIsKept, "a name inside a version is KEPT")                                                    \
    X(ANameInsideAProgramNameIsKept, "a name inside a program name is KEPT")                                           \
    X(NoLeakInUbuntuX, "no leak in 'ubuntu_x'")                                                                        \
    X(NoLeakInUbuntuMac, "no leak in 'ubuntu-mac'")                                                                    \
    X(NoLeakInUbuntuLocal, "no leak in 'ubuntu.local'")                                                                \
    X(NoLeakInUbuntuH, "no leak in 'ubuntu@h'")                                                                        \
    X(NoLeakInHomeUbuntu, "no leak in '/home/ubuntu/'")                                                                \
    X(NoLeakInCUsersUbuntu, "no leak in 'C:\\\\Users\\\\ubuntu\\\\'")                                                  \
    X(NoLeakInDssX, "no leak in 'dss_x'")                                                                              \
    X(NoLeakInDssMac, "no leak in 'dss-mac'")                                                                          \
    X(NoLeakInDssLocal, "no leak in 'dss.local'")                                                                      \
    X(NoLeakInXDssExample, "no leak in 'x@dss.example:'")                                                              \
    X(NoLeakInSshDss, "no leak in 'ssh dss'")                                                                          \
    X(PosixABareWordInAnotherCaseIsKept, "posix: a bare word in another case is kept")                                 \
    X(PosixHostInAnotherCaseIsMasked, "posix: @HOST in another case is masked")                                        \
    X(PosixHostDomainEveryLabelIsMasked, "posix: HOST.domain, every label, is masked")                                 \
    X(PosixAUncHostInAnotherCaseIsMasked, "posix: a UNC host in another case is masked")                               \
    X(PosixAUrlHostInAnotherCaseIsMasked, "posix: a URL host in another case is masked")                               \
    X(PosixAnSshLineMasksTheHostInAnyCase, "posix: an ssh line masks the host in any case")                            \
    X(PosixHostPortInAnotherCaseIsMasked, "posix: host:port in another case is masked")                                \
    X(ANameFollowedByIsACScope, "a name followed by :: is a C++ scope")                                                \
    X(ButNotOnAnRsyncLine, "...but not on an rsync line")                                                              \
    X(EveryLabelAfterThisHostSOwn, "every label after this host's own")                                                \
    X(WindowsABareHostWordInAnotherCaseIsMasked, "windows: a bare host word in another case is masked")                \
    X(WindowsABareAccountWordInAnotherCaseIsMasked, "windows: a bare account word in another case is masked")          \
    X(PosixABareAccountWordInAnotherCaseIsKept, "posix: a bare account word in another case is kept")                  \
    X(AWorktreeSMainCheckoutIsADeclaredTree, "a worktree's main checkout is a declared tree")                          \
    X(TheItemsUserAddressAndFirstLabelAreRead, "the items' USER, ADDRESS and first label are read")                    \
    X(DeclaredRemoteacctLoggedIn, "declared: 'remoteacct logged in'")                                                  \
    X(DeclaredMacacctOwnsIt, "declared: 'macacct owns it'")                                                            \
    X(DeclaredVps7IsUp, "declared: 'vps7 is up'")                                                                      \
    X(DeclaredVps7ExampleNetAnswered, "declared: 'VPS7.EXAMPLE.NET answered'")                                         \
    X(DeclaredRemoteacctIsAnotherWord, "declared: 'Remoteacct is another word'")                                       \
    X(DeclaredSshItem7CopySynced, "declared: 'ssh item7: copy synced'")                                                \
    X(AnUnreadableEnvIsRefusedNamingItsItem, "an unreadable .env is refused, naming its item")                         \
    X(KeptTheTreeNamesItBinSynSynExamplesRunner, "KEPT, the tree names it: 'bin/syn/syn_examples_runner'")             \
    X(KeptTheTreeNamesItLoadingSrcSynConfigXJson, "KEPT, the tree names it: 'loading src/syn-config/x.json'")          \
    X(KeptTheTreeNamesItRunningSynExamplesRunner, "KEPT, the tree names it: 'running syn_examples_runner'")            \
    X(KeptTheTreeNamesItTreeBuildV1BinSyn, "KEPT, the tree names it: '<tree>/build/v1/bin/syn/'")                      \
    X(MaskedAHostPositionOrNoTreeFactUbuntuSynLs, "MASKED, a host position or no tree fact: 'ubuntu@syn:~$ ls'")       \
    X(MaskedAHostPositionOrNoTreeFactLinuxSyn680, "MASKED, a host position or no tree fact: 'Linux syn 6.8.0'")        \
    X(MaskedAHostPositionOrNoTreeFactSshSyn, "MASKED, a host position or no tree fact: 'ssh syn'")                     \
    X(MaskedAHostPositionOrNoTreeFactSynLocal, "MASKED, a host position or no tree fact: 'syn.local'")                 \
    X(MaskedAHostPositionOrNoTreeFactSynMac, "MASKED, a host position or no tree fact: 'syn-mac'")                     \
    X(MaskedAHostPositionOrNoTreeFactXSynExample, "MASKED, a host position or no tree fact: 'x@syn.example:'")         \
    X(MaskedAHostPositionOrNoTreeFactSynShare, "MASKED, a host position or no tree fact: '//syn/share'")               \
    X(MaskedAHostPositionOrNoTreeFactHttpSyn8080, "MASKED, a host position or no tree fact: 'http://syn:8080/'")       \
    X(MaskedAHostPositionOrNoTreeFactTheSynPrefix, "MASKED, a host position or no tree fact: 'the syn prefix'")        \
    X(MaskedAHostPositionOrNoTreeFactLibSynX, "MASKED, a host position or no tree fact: 'lib/syn/x'")                  \
    X(MaskedAHostPositionOrNoTreeFactSynNothingHere, "MASKED, a host position or no tree fact: 'syn/nothing-here'")    \
    X(MaskedANameOnlyWhatGitIgnoresHoldsIsNoTreeFactSynLegcopy,                                                        \
      "MASKED, a name only what git ignores holds is no tree fact: 'syn-legcopy'")                                     \
    X(MaskedANameOnlyWhatGitIgnoresHoldsIsNoTreeFactSynWorker,                                                         \
      "MASKED, a name only what git ignores holds is no tree fact: 'syn-worker'")                                      \
    X(MaskedANameOnlyWhatGitIgnoresHoldsIsNoTreeFactSynScratch,                                                        \
      "MASKED, a name only what git ignores holds is no tree fact: 'syn-scratch'")                                     \
    X(AGitignoreTheOneReaderRefusesKeepsNothingByK, "a .gitignore the one reader refuses keeps nothing by (K)")        \
    X(WithoutATreeNothingIsKept, "without a tree nothing is kept")                                                     \
    X(TheAccountIsNeverKeptTreeOrNot, "the account is never kept, tree or not")                                        \
    X(AWalkCutShortKeepsFewerNamesNeverMore, "a walk cut short keeps fewer names, never more")                         \
    X(TheTreeSTextIsWalkedOnlyWhenADottedCompoundAsks, "the tree's TEXT is walked only when a dotted compound asks")   \
    X(KeptTheTreeSTextSpellsItSynDevOps, "KEPT, the tree's text spells it: 'SYN.DevOps'")                              \
    X(KeptTheTreeSTextSpellsItRanSynMachoArm64Exit, "KEPT, the tree's text spells it: 'ran syn.macho_arm64_exit'")     \
    X(MaskedACompoundTheTreeSTextDoesNotGiveSynCorpIsUp,                                                               \
      "MASKED, a compound the tree's text does not give: 'syn.corp is up'")                                            \
    X(MaskedACompoundTheTreeSTextDoesNotGiveSynDevops,                                                                 \
      "MASKED, a compound the tree's text does not give: 'syn.devops'")                                                \
    X(MaskedACompoundTheTreeSTextDoesNotGiveSynSecretRan,                                                              \
      "MASKED, a compound the tree's text does not give: 'syn.secret ran'")                                            \
    X(MaskedACompoundTheTreeSTextDoesNotGiveSynFixture,                                                                \
      "MASKED, a compound the tree's text does not give: 'syn.fixture'")                                               \
    X(MaskedACompoundTheTreeSTextDoesNotGiveSynEnvtoken,                                                               \
      "MASKED, a compound the tree's text does not give: 'syn.envtoken'")                                              \
    X(MaskedACompoundOnlyAFileGitIgnoresSpellsSynScratchnote,                                                          \
      "MASKED, a compound only a file git ignores spells: 'syn.scratchnote'")                                          \
    X(MaskedACompoundOnlyAFileGitIgnoresSpellsSynToplocal,                                                             \
      "MASKED, a compound only a file git ignores spells: 'syn.toplocal'")                                             \
    X(MaskedAKnownNameTheTreeSTextSpellsPingSynExampleNow,                                                             \
      "MASKED, a known name the tree's text spells: 'ping syn.example now'")                                           \
    X(MaskedAKnownNameTheTreeSTextSpellsPingSynLabExampleNow,                                                          \
      "MASKED, a known name the tree's text spells: 'ping syn.lab.example now'")                                       \
    X(MaskedAKnownNameTheTreeSTextSpellsPingMailSynExampleNow,                                                         \
      "MASKED, a known name the tree's text spells: 'ping mail.syn.example now'")                                      \
    X(ATextWalkCutShortKeepsFewerCompoundsNeverMore, "a text walk cut short keeps fewer compounds, never more")        \
    X(OnAHostNamedDssTheTreeSDssMirIsKept, "on a host named `dss`, the tree's dss.mir is KEPT")                        \
    X(AndADssLabelTheTreeDoesNotSpellIsMasked, "...and a dss.<label> the tree does not spell is masked")               \
    X(NoGetpassTheHomeNamesTheAccount, "no getpass: the home names the account")                                       \
    X(NoAccountAtAllIsRefused, "no account at all is refused")                                                         \
    X(ADetailIsWithheldWholeWhenNoAccountCanBeNamed, "a detail is WITHHELD whole when no account can be named")        \
    X(APlaceIsMarkedLikeTheTreeInEverySlashSpelling, "a place is marked like the tree, in every slash spelling")       \
    X(TheLongestFirstSoAPlaceInsideTheHomeKeepsItsOwnMark,                                                             \
      "...the longest first, so a place inside the home keeps its own mark")                                           \
    X(ADetailIsRedactedWholeThenCutNoPieceOfANameSurvivesEitherEnd,                                                    \
      "a detail is redacted WHOLE, then cut: no piece of a name survives either end")                                  \
    X(ThisHostSAndTheDeclaredNamesNeverSurvive, "this host's and the declared names never survive")                    \
    X(TheFilterAnswersEachLineBeforeTheStreamEnds, "the filter answers each line before the stream ends")              \
    X(TheFilterRedactsEveryLineAndExits0, "the filter redacts every line and exits 0")                                 \
    X(TheCommandLineFilterNamesNoTree, "the command-line filter names no tree")                                        \
    X(TheFilterReadsAFileArgument, "the filter reads a file argument")

#define DSS_REDACT_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_REDACT_SELFTEST_ARMS(DSS_REDACT_SELFTEST_ARM_NAME)};
#undef DSS_REDACT_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::redact_selftest::kCommand,
    .workingDirectory = dss::redact_selftest::kWorkingDirectory,
    .standardOutput   = dss::redact_selftest::kStandardOutput,
    .marker           = "redact self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_REDACT_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_REDACT_SELFTEST_CASE(caseName, armName) \
    TEST(RedactSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_REDACT_SELFTEST_ARMS(DSS_REDACT_SELFTEST_CASE)
#undef DSS_REDACT_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(RedactSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(RedactSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
