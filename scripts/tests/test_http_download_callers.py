#!/usr/bin/env python3
"""Exercise production redirect/fallback/finalization with authored engine doubles.

The real client parser and completion handlers are extracted. Tests verify their
wire commands, UDP-relative names, disconnect handling and filesystem commit
behavior without requiring game assets or a renderer.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    match = re.search(r"^(?:static bool|void __cdecl) " + name + r"\([^\n]*\)\n\{", source, re.MULTILINE)
    if not match:
        raise ValueError(f"production function not found: {name}")
    depth = 1
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|[{}]', source[match.end():], re.DOTALL):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():match.end()+token.end()].replace("__cdecl", "")
    raise ValueError(f"production function not terminated: {name}")


HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
constexpr int CON_CHANNEL_CLIENT = 1, ERR_DROP = 1;
enum dlStatus_t { DL_CONTINUE, DL_DONE, DL_FAILED };
struct msg_t { std::string url; int size, flags, read; };
struct {
    char originalDownloadName[260], downloadName[260], downloadTempName[260];
    int downloadSize, downloadFlags, download;
    bool wwwDlInProgress, wwwDlDisconnected;
} cls;
struct { int cl_downloadSize, cl_downloadCount; char cl_downloadName[64]; } legacyHacks;
struct Dvar { union { const char* string; int integer; bool enabled; } current; } homepath, httpEnabled;
Dvar* fs_homepath = &homepath;
Dvar* cl_wwwDownload = &httpEnabled;
bool autoupdateStarted, beginResult, motd, opened, cleared;
int loopResult, cancels, nexts, motdFinished, drops, begins;
std::vector<std::string> commands, buffered;
std::string root, beganPath, beganUrl, printed;
void I_strncpyz(char* out,const char* in,size_t size) { assert(size); std::snprintf(out,size,"%s",in); }
void Com_sprintf(char* out,size_t size,const char* format,...) {
    va_list args; va_start(args,format); std::vsnprintf(out,size,format,args); va_end(args);
}
void Com_DPrintf(int,const char*,...) {}
void Com_Printf(int,const char* format,...) {
    char buffer[1024]; va_list args; va_start(args,format);
    std::vsnprintf(buffer,sizeof(buffer),format,args); va_end(args); printed += buffer;
}
const char* Dvar_GetString(const char* key) { assert(std::string(key)=="fs_homepath"); return root.c_str(); }
char* MSG_ReadString(msg_t* msg) { return msg->url.data(); }
int MSG_ReadLong(msg_t* msg) { return msg->read++ ? msg->flags : msg->size; }
void FS_BuildOSPath(char* base,char* game,char* qpath,char* out) {
    assert(base==root.c_str()); std::snprintf(out,260,"%s/%s/%s",base,game,qpath);
}
void Sys_OpenURL(const char*,int) { opened=true; }
void Cbuf_AddText(int client,const char* text) { assert(client==0); buffered.emplace_back(text); }
void CL_AddReliableCommand(int client,const char* text) { assert(client==0); commands.emplace_back(text); }
void DL_CancelDownload() { ++cancels; }
int DL_BeginDownload(char* file,char* url) { ++begins; beganPath=file; beganUrl=url; return beginResult; }
int DL_DownloadLoop() { return loopResult; }
bool DL_DLIsMotd() { return motd; }
void CL_FinishMotdDownload() { ++motdFinished; }
void CL_NextDownload(int client) { assert(client==0); ++nexts; }
void CL_ClearStaticDownload() {
    assert(!cls.wwwDlDisconnected); cleared=true; cls.wwwDlInProgress=false;
    cls.downloadName[0]=cls.downloadTempName[0]=0; DL_CancelDownload();
}
void Com_Error(int type,const char* message,...) {
    assert(type==ERR_DROP); ++drops; printed += message; throw std::runtime_error("drop");
}
@PARSE@
@COMMIT@
@LOOP@
namespace fs=std::filesystem;
unsigned cases;
void pass(const char* name) { ++cases; std::cout << "PASS " << name << '\n'; }
void reset() {
    cls={}; legacyHacks={}; autoupdateStarted=false; beginResult=true; motd=false;
    opened=cleared=false; loopResult=DL_CONTINUE;
    cancels=nexts=motdFinished=drops=begins=0; commands.clear(); buffered.clear(); printed.clear();
    I_strncpyz(cls.downloadName,"mods/test/map.iwd",sizeof(cls.downloadName));
    I_strncpyz(cls.downloadTempName,"mods/test/map.iwd.tmp",sizeof(cls.downloadTempName));
    I_strncpyz(legacyHacks.cl_downloadName,"mods/test/map.iwd",sizeof(legacyHacks.cl_downloadName));
    legacyHacks.cl_downloadCount=999;
    homepath.current.string=root.c_str();
    httpEnabled.current.enabled=true;
}
void parse(int flags) {
    msg_t message{"http://tester:example@server.invalid/maps/map.iwd",65536,flags,0};
    CL_ParseWWWDownload(0,&message);
}
void fallback() {
    assert(std::string(cls.downloadName)=="mods/test/map.iwd");
    assert(std::string(cls.downloadTempName)=="mods/test/map.iwd.tmp");
    assert(!cls.wwwDlInProgress && !cls.wwwDlDisconnected);
    assert(legacyHacks.cl_downloadCount==0);
    assert(commands.back()=="wwwdl fail" && nexts==0 && drops==0);
    assert(printed.find("example@") == std::string::npos);
}
std::string contents(const fs::path& file) {
    std::ifstream in(file,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void temp(const char* data="new download") {
    std::ofstream out(cls.downloadTempName,std::ios::binary); out << data;
}
int main(int argc,char** argv) {
    assert(argc==2); root=argv[1]; fs::create_directories(root+"/mods/test");
    reset(); parse(0);
    assert(cls.wwwDlInProgress && !cls.wwwDlDisconnected);
    assert(commands==std::vector<std::string>{"wwwdl ack"});
    assert(beganPath==root+"/mods/test/map.iwd.tmp");
    assert(beganUrl=="http://tester:example@server.invalid/maps/map.iwd");
    assert(legacyHacks.cl_downloadSize==65536);
    pass("connected redirect starts with absolute temporary name");
    reset(); parse(1);
    assert(cls.wwwDlInProgress && cls.wwwDlDisconnected);
    assert((commands==std::vector<std::string>{"wwwdl ack","wwwdl bbl8r"}));
    pass("successful disconnected redirect requests disconnect");
    reset(); beginResult=false; parse(0); fallback();
    assert(cancels==1 && begins==1 && buffered.empty());
    pass("initialization failure restores UDP fallback paths");
    reset(); beginResult=false; parse(1); fallback();
    assert((commands==std::vector<std::string>{"wwwdl ack","wwwdl fail"}));
    pass("failed initialization never disconnects the client");
    reset(); parse(2);
    assert(opened && begins==0 && cancels==1 && !cls.wwwDlInProgress);
    assert(buffered==std::vector<std::string>{"quit\n"});
    assert(commands==std::vector<std::string>{"wwwdl bbl8r"});
    pass("explicit browser redirect retains existing behavior");
    for (int flags : {0,1,2}) {
        reset(); httpEnabled.current.enabled=false; parse(flags); fallback();
        assert(!opened && begins==0 && buffered.empty());
        assert((commands==std::vector<std::string>{"wwwdl ack","wwwdl fail"}));
        pass(flags==0 ? "local HTTP disable rejects connected redirect" :
             flags==1 ? "local HTTP disable never disconnects" :
                        "local HTTP disable never opens a browser");
    }

    reset(); parse(0); commands.clear(); CL_WWWDownload();
    assert(cls.wwwDlInProgress && nexts==0 && commands.empty() && drops==0);
    pass("incomplete transfer leaves completion state untouched");
    loopResult=DL_FAILED; CL_WWWDownload(); fallback();
    pass("transfer failure restores relative paths before UDP fallback");
    reset(); parse(1); commands.clear(); loopResult=DL_FAILED;
    try { CL_WWWDownload(); assert(!"disconnected failure must drop"); }
    catch (const std::runtime_error&) {}
    assert(cleared && drops==1 && !cls.wwwDlDisconnected && commands.empty());
    assert(printed.find("tester")==std::string::npos);
    pass("disconnected transfer failure clears state and drops safely");

    reset(); parse(0); temp(); commands.clear(); loopResult=DL_DONE;
    { std::ofstream old(root+"/mods/test/map.iwd"); old << "old data"; }
    CL_WWWDownload();
    assert(contents(root+"/mods/test/map.iwd")=="new download");
    assert(!fs::exists(beganPath) && !cls.wwwDlInProgress && !cls.downloadName[0] && !cls.downloadTempName[0]);
    assert(commands==std::vector<std::string>{"wwwdl done"} && nexts==1);
    assert(!legacyHacks.cl_downloadName[0] && buffered.empty());
    pass("completed download replaces the target before acknowledging success");
    reset(); parse(1); temp(); commands.clear(); loopResult=DL_DONE; CL_WWWDownload();
    assert(buffered==std::vector<std::string>{"reconnect\n"} && commands.empty() && nexts==1);
    pass("disconnected completion reconnects once");
    reset(); parse(1); temp(); commands.clear(); autoupdateStarted=true; loopResult=DL_DONE; CL_WWWDownload();
    assert(buffered.empty() && commands.empty() && nexts==1);
    pass("autoupdate completion preserves its separate reconnect policy");

    reset(); parse(0); temp(); commands.clear(); loopResult=DL_DONE;
    fs::remove(root+"/mods/test/map.iwd"); fs::create_directory(root+"/mods/test/map.iwd");
    CL_WWWDownload(); fallback();
    assert(!fs::exists(beganPath) && fs::is_directory(root+"/mods/test/map.iwd"));
    pass("finalization failure preserves target and never acknowledges success");
    reset(); parse(1); temp(); commands.clear(); loopResult=DL_DONE;
    try { CL_WWWDownload(); assert(!"disconnected finalization failure must drop"); }
    catch (const std::runtime_error&) {}
    assert(cleared && drops==1 && commands.empty() && !fs::exists(beganPath));
    pass("disconnected finalization failure cannot report success");
    fs::remove(root+"/mods/test/map.iwd");

    reset(); parse(0); temp(); commands.clear(); motd=true; loopResult=DL_DONE; CL_WWWDownload();
    assert(motdFinished==1 && nexts==0 && commands.empty() && !cls.wwwDlInProgress);
    assert(fs::exists(beganPath)); fs::remove(beganPath);
    pass("legacy MOTD success uses only the MOTD completion path");
    reset(); parse(0); commands.clear(); motd=true; loopResult=DL_FAILED; CL_WWWDownload();
    assert(motdFinished==0 && nexts==0 && commands.empty() && !cls.wwwDlInProgress);
    pass("legacy MOTD failure does not install or acknowledge a mod");
    std::cout << cases << " HTTP caller regression cases passed\n";
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root",type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    parse=(args.source_root/"src/client_mp/cl_parse_mp.cpp").read_text()
    loop=(args.source_root/"src/client_mp/cl_main_mp.cpp").read_text()
    harness=(HARNESS.replace("@PARSE@",extract(parse,"CL_ParseWWWDownload"))
             .replace("@COMMIT@",extract(loop,"CL_CommitWWWDownload"))
             .replace("@LOOP@",extract(loop,"CL_WWWDownload")))
    with tempfile.TemporaryDirectory(prefix="kisakcod-http-callers-") as temporary:
        root=Path(temporary); source=root/"callers.cpp"; source.write_text(harness)
        executable=root/"callers"
        compiler=shlex.split(os.environ.get("CXX","c++"))
        subprocess.run(compiler+["-std=c++20","-O1","-g","-Wall","-Wextra","-Werror",str(source),"-o",str(executable)],check=True)
        subprocess.run([str(executable),str(root/"game")],check=True)


if __name__=="__main__":
    main()
