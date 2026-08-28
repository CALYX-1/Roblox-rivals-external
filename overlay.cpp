// overlay.cpp - Rivals external overlay, v1.
// A transparent, click-through, topmost layered window with a software ARGB
// rasterizer, world-to-screen, and a DataSource seam. v1 runs on a MOCK camera
// and orbiting fake entities to prove projection + rendering before the memory
// reader exists. Keys: ESC quit, F8 dump backbuffer to overlay_dump.bmp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <utility>
#include <unordered_map>
#include <set>
#include <thread>
#include <atomic>
#include <algorithm>
#include <timeapi.h>
#include <cstdarg>
#pragma comment(lib, "winmm.lib")

// ----------------------------- math -----------------------------
struct Vec3 { float x,y,z; };
static Vec3 sub(Vec3 a,Vec3 b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float dot(Vec3 a,Vec3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3 cross(Vec3 a,Vec3 b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static Vec3 norm(Vec3 a){ float l=sqrtf(dot(a,a)); if(l<1e-6f) return {0,0,0}; return {a.x/l,a.y/l,a.z/l}; }
static Vec3 addv(Vec3 a,Vec3 b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static Vec3 muls(Vec3 a,float s){ return {a.x*s,a.y*s,a.z*s}; }

// ESP display options — toggled from the in-overlay menu.
struct Options {
    bool esp=true, box=true, name=true, health=true, snapline=true, snapCenter=false;
    bool hideTeam=true;    // drop players whose TeamID matches yours
    bool envFilter=true;   // only players whose EnvironmentID matches yours
    // ---- aimbot ----
    bool aim=false;            // master switch
    int  aimMode=1;            // 0 always on, 1 hold the bind, 2 toggle with the bind
    int  aimKey=VK_RBUTTON;    // activation bind (MB2 = aim-down-sights in Rivals)
    int  aimPart=0;            // 0 head, 1 torso
    bool aimFov=true;          // only consider targets inside a screen-space circle
    int  aimFovPx=90;          // its radius, pixels
    bool aimFovDraw=true;      // draw that circle
    int  aimSmooth=8;          // 1 = snap, 20 = lazy drift
    int  aimTarget=0;          // 0 nearest-to-crosshair, 1 lock one target in the FOV
    int  aimMaxDist=0;         // studs; 0 = unlimited
    bool aimWall=true;         // skip targets with a wall between them and the camera
    bool espVis=true;          // dim the box of an enemy the wall check says is covered
};
static Options g_opt;

// ----------------------------- settings file -----------------------------
// Every toggle used to reset on launch. config.ini sits next to the exe and is
// rewritten whenever something changes or the menu closes.
static const char* CFG_PATH = "config.ini";
static uint32_t g_dmVftRva = 0;    // cached DataModel vftable RVA - always RE-VERIFIED
static bool     g_cfgDirty = false;
struct CfgB { const char* k; bool* v; };
struct CfgI { const char* k; int*  v; int lo, hi; };
static CfgB g_cfgB[] = {
    {"esp",&g_opt.esp},{"box",&g_opt.box},{"names",&g_opt.name},{"health",&g_opt.health},
    {"snaplines",&g_opt.snapline},{"snapCenter",&g_opt.snapCenter},
    {"hideTeam",&g_opt.hideTeam},{"envFilter",&g_opt.envFilter},
    {"aim",&g_opt.aim},{"aimFov",&g_opt.aimFov},{"aimFovDraw",&g_opt.aimFovDraw},
    {"aimWall",&g_opt.aimWall},{"espVis",&g_opt.espVis},
};
static CfgI g_cfgI[] = {
    {"aimMode",&g_opt.aimMode,0,2},{"aimKey",&g_opt.aimKey,1,254},{"aimPart",&g_opt.aimPart,0,1},
    {"aimFovPx",&g_opt.aimFovPx,10,800},{"aimSmooth",&g_opt.aimSmooth,1,20},
    {"aimTarget",&g_opt.aimTarget,0,1},{"aimMaxDist",&g_opt.aimMaxDist,0,5000},
};

// The measured per-zoom gains, persisted so he does not re-learn them from the
// 0.15 seed on every launch. Written as `gain=<fov>|<gx>|<gy>` lines.
struct GainBucket { float fov; double gx, gy; int n; };
static GainBucket g_gb[20];
static int g_gbN=0;
static const int  GB_MAX=20;
// Measured after a real session: he ended up with 47.364, 46.564 and 45.008 as
// three separate buckets carrying the same gain - they are all the same ADS,
// caught at slightly different points of the tween. 0.75 deg was too tight.
static const float GB_TOL=2.5f;
static void cfgSave(){
    FILE* f=fopen(CFG_PATH,"w");
    if(!f) return;
    fputs("# rivals-external overlay settings (rewritten by the overlay)\n",f);
    for(auto& b : g_cfgB) fprintf(f,"%s=%d\n",b.k,*b.v?1:0);
    for(auto& i : g_cfgI) fprintf(f,"%s=%d\n",i.k,*i.v);
    for(int i=0;i<g_gbN;i++)
        fprintf(f,"gain=%.3f|%.6f|%.6f\n",g_gb[i].fov,g_gb[i].gx,g_gb[i].gy);
    fprintf(f,"dmVftRva=%u\n",g_dmVftRva);
    fclose(f);
}
static void cfgLoad(){
    FILE* f=fopen(CFG_PATH,"r");
    if(!f) return;
    char line[256];
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='#'||line[0]==';') continue;
        char* eq=strchr(line,'='); if(!eq) continue;
        *eq=0;
        char* k=line; while(*k==' '||*k=='\t') k++;
        for(char* t=k+strlen(k); t>k && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
        long v=strtol(eq+1,nullptr,10);
        bool done=false;
        for(auto& b : g_cfgB) if(!strcmp(k,b.k)){ *b.v=(v!=0); done=true; break; }
        if(done) continue;
        for(auto& i : g_cfgI) if(!strcmp(k,i.k)){
            if(v<i.lo) v=i.lo; if(v>i.hi) v=i.hi;
            *i.v=(int)v; done=true; break;
        }
        if(done) continue;
        if(!strcmp(k,"dmVftRva")){ g_dmVftRva=(uint32_t)strtoul(eq+1,nullptr,10); continue; }
        if(!strcmp(k,"gain") && g_gbN<20){
            float fv=0; double gx=0, gy=0;
            if(sscanf(eq+1,"%f|%lf|%lf",&fv,&gx,&gy)==3 && fv>1 && fv<180 &&
               fabs(gx)>1e-4 && fabs(gx)<10 && fabs(gy)>1e-4 && fabs(gy)<10){
                g_gb[g_gbN].fov=fv; g_gb[g_gbN].gx=gx; g_gb[g_gbN].gy=gy; g_gb[g_gbN].n=0;
                g_gbN++;
            }
        }
    }
    fclose(f);
}
// Which data source is actually feeding the overlay: MEM (no executor), FEED, MOCK.
static const char* g_srcTag = "MOCK";

// A camera described the way Roblox exposes one: position + basis (right/up/look)
// + vertical FOV + viewport. This is exactly what the memory reader will fill.
struct Camera {
    Vec3 pos, right, up, look;   // look = forward (Roblox CFrame LookVector, unit)
    float fovDeg;                // vertical field of view, degrees
    int   w, h;                  // viewport pixels
    int   insetX=0, insetY=0;    // GuiService gui inset (viewport top-left vs client top-left)
};

// World -> screen. Returns false if behind camera. Out is pixel coords + depth.
static bool worldToScreen(const Camera& c, Vec3 world, float& sx, float& sy, float& depth){
    Vec3 d = sub(world, c.pos);
    float cz = dot(d, c.look);            // depth along view direction
    if (cz < 0.01f) return false;         // behind / on the plane
    float cx = dot(d, c.right);
    float cy = dot(d, c.up);
    float tanHalf = tanf(c.fovDeg * 0.5f * 3.14159265f/180.0f);
    float aspect  = (float)c.w / (float)c.h;
    float ndcx = (cx / (cz * tanHalf * aspect));
    float ndcy = (cy / (cz * tanHalf));
    sx = (ndcx * 0.5f + 0.5f) * c.w;
    sy = (1.0f - (ndcy * 0.5f + 0.5f)) * c.h;   // y down in screen space
    depth = cz;
    return true;
}

// ----------------------------- data source seam -----------------------------
struct Entity {
    std::string name;
    float health, maxHealth;
    Vec3 head;    // world pos of box TOP  (head top)
    Vec3 root;    // world pos of box BOTTOM (feet)
    float width=3.2f;  // world width in studs
    bool enemy;
    uint64_t id=0;               // character Model address - stable key for aim lock-on
    // Where the aimbot points. Rivals' real hit parts are HitboxHead / HitboxBody,
    // NOT the visual Head mesh, so those are resolved separately (SS6).
    Vec3 aimHead{}, aimBody{};
    bool hasAimHead=false, hasAimBody=false;
    bool local=false;            // this is us - never drawn
    std::string team, env;       // raw TeamID / EnvironmentID attribute bytes ("" = unset)
    // Wall-check answer, computed ONCE per frame in poll() and then reused by the
    // ESP dimming and the aimbot. It used to be recomputed several
    // times over ~1,470 boxes for every enemy on screen.
    bool  covHead=false, covBody=false;
};
struct Frame { Camera cam; std::vector<Entity> ents; };

// ========================= MEMORY DATA SOURCE =========================
// Reads Rivals' state straight out of RobloxPlayerBeta.exe. No executor, no
// injected module, no hooks, no writes - only ReadProcessMemory from a separate
// process. Offsets are documented in HANDOFF.md section 9; every one of them was
// validated bit-for-bit against the executor oracle before being baked in here.
//
// Nothing is hardcoded to a build: the DataModel is located by scanning the
// module for the vftable whose MSVC RTTI name is "RBX::DataModel", so this
// survives a Roblox update as long as the class keeps its name.
static bool  robloxClient(int& x,int& y,int& w,int& h);   // defined further down
static DWORD robloxPid();                                // and so is this
static HWND  g_rbxWnd=NULL;   // cached by robloxClient(); the aimbot's foreground gate

namespace rv {

// ---- Instance ----------------------------------------------------------
static const uint64_t I_PARENT    = 0x68;   // -> parent Instance
static const uint64_t I_NAME      = 0x70;   // -> interned string record
static const uint64_t I_CHILDREN  = 0x78;   // -> shared_ptr _Ptr -> vector{begin,end,cap}
static const uint64_t S_CHARS     = 0x08;   // inside the name record (SSO while len<=15)
static const uint64_t S_LEN       = 0x18;
// ---- BasePart / Primitive ---------------------------------------------
static const uint64_t P_PRIMITIVE = 0x188;
static const uint64_t PR_ROT      = 0xC8;   // 3x3 row-major, same layout as the Camera's
static const uint64_t PR_POS      = 0xEC;   // 3 floats
static const uint64_t PR_POS2     = 0x134;  // PV #2 position. The Primitive holds TWO
                                            // physics states 0x48 apart (CFrame 0x30 +
                                            // linear/angular velocity 0x18) - a double
                                            // buffer. Writing only one gets overwritten
                                            // from the other on the next step, which is
                                            // why the teleport writes both.
static const uint64_t PR_SIZE     = 0x1BC;  // 3 floats
// ---- Humanoid ----------------------------------------------------------
static const uint64_t H_HEALTH    = 0x190;
static const uint64_t H_MAXHEALTH = 0x1A8;
// ---- Camera ------------------------------------------------------------
static const uint64_t C_ROT       = 0xD8;   // 3x3 row-major, R00..R22
static const uint64_t C_POS       = 0xFC;
static const uint64_t C_FOCUSPOS  = 0x12C;  // Focus tracks the local humanoid
static const uint64_t C_FOV       = 0x140;  // VERTICAL, in RADIANS
// ---- Players / Player --------------------------------------------------
static const uint64_t PS_LOCALPLAYER = 0x130;  // RBX::Players -> the local RBX::Player
static const uint64_t PL_CHARACTER   = 0x298;  // RBX::Player  -> its character Model
// ---- attributes --------------------------------------------------------
// Attributes are NOT stored inside the Instance. Every instance carries a
// vector of optional side-state at +0x38; each 0x10-byte element is
// { void* p, uint16 kind } and kind 0x0F is the attribute container.
static const uint64_t I_STATEVEC    = 0x38;
static const uint64_t SV_ELEM       = 0x10;
static const uint16_t SV_KIND_ATTR  = 0x0F;
static const uint64_t AC_COUNT      = 0x08;   // uint32
static const uint64_t AC_ENTRIES    = 0x18;   // -> Entry[count]
static const uint64_t AE_STRIDE     = 0x58;
static const uint64_t AE_NAME       = 0x00;   // -> interned name record
static const uint64_t AE_TYPE       = 0x08;   // -> Reflection::Type (its +0x08 names it)
static const uint64_t AE_VALUE      = 0x18;   // std::string: buf16, +0x10 size, +0x18 cap

#pragma pack(push,1)
struct COL { uint32_t sig, offset, cdOffset, pTD, pCHD, pSelf; };
struct CHD { uint32_t sig, attr, numBase, pBCA; };
struct BCD { uint32_t pTD, numContained; int32_t mdisp, pdisp, vdisp; uint32_t attr; };
#pragma pack(pop)

static HANDLE    g_h    = NULL;
static DWORD     g_pid  = 0;
static uintptr_t g_base = 0;
static uint64_t  g_size = 0;
static std::vector<uint8_t> g_img;                       // module snapshot
static std::unordered_map<uint64_t,std::string> g_vftName;   // vftable -> class, memoised

static inline bool rd(uint64_t a, void* b, size_t n){
    SIZE_T got=0;
    return g_h && ReadProcessMemory(g_h,(LPCVOID)a,b,n,&got) && got==n;
}
static inline bool inMod(uint64_t v){ return g_size && v>=g_base && v<g_base+g_size; }
static inline bool mrd(uint64_t a, void* b, size_t n){
    if(!g_img.empty() && a>=g_base && a+n<=g_base+g_size){
        memcpy(b, g_img.data()+(size_t)(a-g_base), n); return true;
    }
    return rd(a,b,n);
}

// NOTHING IN THIS PROGRAM WRITES TO RIVALS.
// There was a write path for a while - silent aim redirected the shot by writing
// Camera+0xD8 - and deleting that feature took the last WriteProcessMemory with
// it. The handle this process asks Windows for is read-only again, which is the
// property the whole external track exists to keep (11.6).
struct Region { uintptr_t base; size_t size; DWORD type; DWORD prot; };
static bool readable(DWORD p){
    if(p & PAGE_GUARD) return false;
    DWORD m = p & 0xFF;
    return m==PAGE_READONLY||m==PAGE_READWRITE||m==PAGE_WRITECOPY||
           m==PAGE_EXECUTE_READ||m==PAGE_EXECUTE_READWRITE||m==PAGE_EXECUTE_WRITECOPY;
}
static std::vector<Region> regions(){
    std::vector<Region> v; MEMORY_BASIC_INFORMATION mbi{}; uintptr_t a=0;
    while(a < 0x7FFFFFFFFFFFull && VirtualQueryEx(g_h,(LPCVOID)a,&mbi,sizeof(mbi))){
        uintptr_t rb=(uintptr_t)mbi.BaseAddress; size_t rs=mbi.RegionSize;
        if(!rs) break;
        if(mbi.State==MEM_COMMIT && readable(mbi.Protect))
            v.push_back({rb,rs,mbi.Type,mbi.Protect});
        a = rb + rs;
    }
    return v;
}

static bool attach(){
    if(g_h) return true;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if(Process32FirstW(snap,&pe)) do{
        if(_wcsicmp(pe.szExeFile,L"RobloxPlayerBeta.exe")==0){ g_pid=pe.th32ProcessID; break; }
    } while(Process32NextW(snap,&pe));
    CloseHandle(snap);
    if(!g_pid) return false;
    HANDLE ms=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,g_pid);
    if(ms!=INVALID_HANDLE_VALUE){
        MODULEENTRY32W me{}; me.dwSize=sizeof(me);
        if(Module32FirstW(ms,&me)) do{
            if(_wcsicmp(me.szModule,L"RobloxPlayerBeta.exe")==0){
                g_base=(uintptr_t)me.modBaseAddr; g_size=me.modBaseSize; break;
            }
        } while(Module32NextW(ms,&me));
        CloseHandle(ms);
    }
    // Read-only access is all this ever needs.
    g_h=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,FALSE,g_pid);
    if(!g_h || !g_base){ if(g_h){CloseHandle(g_h); g_h=NULL;} g_pid=0; return false; }
    return true;
}

static void loadImage(){
    if(!g_img.empty() || !g_size) return;
    g_img.assign((size_t)g_size, 0);
    for(auto& r : regions()){
        if(r.base<g_base || r.base>=g_base+g_size) continue;
        size_t off=(size_t)(r.base-g_base), n=r.size;
        if(off+n>g_img.size()) n=g_img.size()-off;
        SIZE_T got=0; ReadProcessMemory(g_h,(LPCVOID)r.base,g_img.data()+off,n,&got);
    }
}

// ".?AVPrimitive@RBX@@" -> "RBX::Primitive"
static void demangle(const char* m, char* out, size_t n){
    if(strncmp(m,".?A",3)!=0){ snprintf(out,n,"%s",m); return; }
    const char* p=m+3; if(*p=='V'||*p=='U') ++p;
    std::vector<std::string> parts; std::string cur;
    for(; *p; ++p){
        if(*p=='@'){ if(cur.empty()) break; parts.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    if(!cur.empty()) parts.push_back(cur);
    std::string s;
    for(size_t i=parts.size(); i-- > 0;){ s+=parts[i]; if(i) s+="::"; }
    snprintf(out,n,"%s", s.empty()?m:s.c_str());
}

static bool rttiName(uint64_t vft, char* out, size_t n, COL* co=nullptr){
    uint64_t colp=0;
    if(!mrd(vft-8,&colp,8) || !inMod(colp)) return false;
    COL c{};
    if(!mrd(colp,&c,sizeof(c))) return false;
    if(c.sig!=1) return false;
    if(colp - c.pSelf != g_base) return false;      // rejects false positives
    if(c.pTD>=g_size || c.pCHD>=g_size) return false;
    char raw[320]{};
    if(!mrd(g_base+c.pTD+0x10,raw,sizeof(raw)-1)) return false;
    if(strncmp(raw,".?A",3)!=0) return false;
    demangle(raw,out,n);
    if(co) *co=c;
    return true;
}

// Class of the object at `inst`, memoised by vftable so it costs one read after
// the first time any object of that class is seen.
static const std::string& classOf(uint64_t inst){
    static const std::string empty;
    uint64_t vft=0;
    if(!rd(inst,&vft,8) || !inMod(vft)) return empty;
    auto it=g_vftName.find(vft);
    if(it!=g_vftName.end()) return it->second;
    char nm[320];
    std::string s = rttiName(vft,nm,sizeof(nm)) ? nm : std::string();
    return g_vftName.emplace(vft,std::move(s)).first->second;
}

static bool nameOf(uint64_t inst, std::string& out){
    uint64_t rec=0;
    if(!rd(inst+I_NAME,&rec,8) || rec<0x10000) return false;
    uint64_t len=0;
    if(!rd(rec+S_LEN,&len,8) || len>4096) return false;
    char buf[264]{};
    size_t want=(size_t)len; if(want>sizeof(buf)-1) want=sizeof(buf)-1;
    if(len<=15){                                   // SSO: text is inline
        if(want && !rd(rec+S_CHARS,buf,want)) return false;
    } else {                                       // longer: the buffer holds a pointer
        uint64_t heap=0;
        if(!rd(rec+S_CHARS,&heap,8) || heap<0x10000) return false;
        if(!rd(heap,buf,want)) return false;
    }
    buf[want]=0; out=buf; return true;
}

static bool childrenOf(uint64_t inst, std::vector<uint64_t>& kids){
    kids.clear();
    uint64_t vec=0;
    if(!rd(inst+I_CHILDREN,&vec,8) || vec<0x10000) return false;
    uint64_t be[2]={0,0};
    if(!rd(vec,be,16)) return false;
    if(be[1]<be[0]) return false;
    uint64_t span=be[1]-be[0];
    // elements are 16-byte std::shared_ptr<Instance> {_Ptr,_Rep}; only _Ptr matters
    if(span%16 || span>16ull*100000) return false;
    size_t cnt=(size_t)(span/16);
    if(!cnt) return true;
    std::vector<uint64_t> raw(cnt*2);
    if(!rd(be[0],raw.data(),cnt*16)) return false;
    kids.reserve(cnt);
    for(size_t i=0;i<cnt;i++) if(raw[i*2]>0x10000) kids.push_back(raw[i*2]);
    return true;
}

static uint64_t childByName(uint64_t inst, const char* want){
    std::vector<uint64_t> kids;
    if(!childrenOf(inst,kids)) return 0;
    std::string nm;
    for(size_t i=0;i<kids.size();i++)
        if(nameOf(kids[i],nm) && nm==want) return kids[i];
    return 0;
}
static uint64_t childByClass(uint64_t inst, const char* want){
    std::vector<uint64_t> kids;
    if(!childrenOf(inst,kids)) return 0;
    for(size_t i=0;i<kids.size();i++)
        if(classOf(kids[i])==want) return kids[i];
    return 0;
}

// Primitive pointer of a BasePart (works for RBX::Part and RBX::MeshPart alike).
static uint64_t primOf(uint64_t part){
    uint64_t p=0;
    if(!part || !rd(part+P_PRIMITIVE,&p,8) || p<0x10000) return 0;
    return p;
}
// A part's full CFrame. The rotation sits 0x24 before the position, exactly like
// the Camera's (C_ROT 0xD8 / C_POS 0xFC) - it is the same G3D::CoordinateFrame
// struct. Verified on a live `Panel` part: identity rotation, size 16x9x1.
static bool primCFrame(uint64_t prim, float rot[9], Vec3& pos){
    if(!prim) return false;
    if(!rd(prim+PR_ROT,rot,36)) return false;
    return rd(prim+PR_POS,&pos,12);
}
static bool primPos(uint64_t prim, Vec3& v){
    float f[3];
    if(!prim || !rd(prim+PR_POS,f,12)) return false;
    v.x=f[0]; v.y=f[1]; v.z=f[2]; return true;
}
static bool primSize(uint64_t prim, Vec3& v){
    float f[3];
    if(!prim || !rd(prim+PR_SIZE,f,12)) return false;
    v.x=f[0]; v.y=f[1]; v.z=f[2]; return true;
}

// ---- bulk readers -------------------------------------------------------
// ReadProcessMemory costs ~20us of syscall overhead REGARDLESS of size, so the
// thing that matters is the NUMBER of calls, not the bytes. One 0x80-byte read
// gets an instance's vftable, name record and children vector together; that
// turned a ~10,000-call rescan into a ~2,000-call one.
struct Hdr { uint64_t vft=0, nameRec=0, childVec=0; bool ok=false; };

static Hdr hdrOf(uint64_t inst){
    Hdr h; uint8_t b[0x80];
    if(inst<0x10000 || !rd(inst,b,sizeof(b))) return h;
    memcpy(&h.vft,      b,            8);
    memcpy(&h.nameRec,  b+I_NAME,     8);
    memcpy(&h.childVec, b+I_CHILDREN, 8);
    h.ok=true; return h;
}

static const std::string& classOfVft(uint64_t vft){
    static const std::string empty;
    if(!inMod(vft)) return empty;
    auto it=g_vftName.find(vft);
    if(it!=g_vftName.end()) return it->second;
    char nm[320];
    std::string s = rttiName(vft,nm,sizeof(nm)) ? nm : std::string();
    return g_vftName.emplace(vft,std::move(s)).first->second;
}

static bool nameFromRec(uint64_t rec, std::string& out){
    if(rec<0x10000) return false;
    uint8_t b[0x28];
    if(!rd(rec,b,sizeof(b))) return false;
    uint64_t len; memcpy(&len,b+S_LEN,8);
    if(len>4096) return false;
    char buf[264]{};
    size_t want=(size_t)len; if(want>sizeof(buf)-1) want=sizeof(buf)-1;
    if(len<=15){                       // SSO: already inside the bytes we read
        memcpy(buf,b+S_CHARS,want);
    } else {
        uint64_t heap; memcpy(&heap,b+S_CHARS,8);
        if(heap<0x10000 || !rd(heap,buf,want)) return false;
    }
    buf[want]=0; out=buf; return true;
}

static bool childrenFromVec(uint64_t vec, std::vector<uint64_t>& kids){
    kids.clear();
    if(vec<0x10000) return false;
    uint64_t be[2]={0,0};
    if(!rd(vec,be,16) || be[1]<be[0]) return false;
    uint64_t span=be[1]-be[0];
    if(span%16 || span>16ull*100000) return false;
    size_t cnt=(size_t)(span/16);
    if(!cnt) return true;
    std::vector<uint64_t> raw(cnt*2);
    if(!rd(be[0],raw.data(),cnt*16)) return false;
    kids.reserve(cnt);
    for(size_t i=0;i<cnt;i++) if(raw[i*2]>0x10000) kids.push_back(raw[i*2]);
    return true;
}

// ---- attributes ---------------------------------------------------------
// Rivals puts the team in the player Attribute "TeamID" (a ONE-BYTE string,
// 0x01 / 0x02 - compare by inequality, never hardcode) and the concurrent
// arena in "EnvironmentID". Both were validated byte-for-byte against the
// executor oracle across a full 30-player server before being wired in here.
//
// Attribute names are interned and shared by every instance in the process,
// so the record pointer identifies the name: memoise on the pointer and the
// steady-state cost of reading a player's attributes is 3 reads, no strings.
static std::unordered_map<uint64_t,std::string> g_attrName;   // name record -> text
static std::unordered_map<uint64_t,std::string> g_typeName;   // Reflection::Type -> "string"...

static const std::string& attrNameOf(uint64_t rec){
    static const std::string empty;
    if(rec<0x10000) return empty;
    auto it=g_attrName.find(rec);
    if(it!=g_attrName.end()) return it->second;
    std::string s;
    if(!nameFromRec(rec,s)) s.clear();
    return g_attrName.emplace(rec,std::move(s)).first->second;
}

// A plain (headerless) MSVC std::string: buf[16], +0x10 size, +0x18 capacity.
static bool stdString(uint64_t addr, std::string& out){
    uint8_t b[0x18];
    if(addr<0x10000 || !rd(addr,b,sizeof(b))) return false;
    uint64_t len; memcpy(&len,b+0x10,8);
    if(len>4096) return false;
    if(len<=15){ out.assign((const char*)b,(size_t)len); return true; }
    uint64_t heap; memcpy(&heap,b,8);
    if(heap<0x10000) return false;
    std::vector<char> t((size_t)len);
    if(!rd(heap,t.data(),(size_t)len)) return false;
    out.assign(t.data(),(size_t)len);
    return true;
}

static const std::string& typeNameOf(uint64_t type){
    static const std::string empty;
    if(!inMod(type)) return empty;
    auto it=g_typeName.find(type);
    if(it!=g_typeName.end()) return it->second;
    uint64_t np=0; std::string s;
    if(rd(type+8,&np,8)) stdString(np,s);
    return g_typeName.emplace(type,std::move(s)).first->second;
}

struct AttrSet { std::string teamId, envId; bool ok=false; };

// Decode one candidate container. Returns false when it does not parse as an
// attribute table, which is what keeps the kind-tag fallback below honest.
static bool attrParse(uint64_t c, AttrSet& out){
    uint32_t count=0; uint64_t entries=0;
    if(!rd(c+AC_COUNT,&count,4) || !rd(c+AC_ENTRIES,&entries,8)) return false;
    if(!count || count>256 || entries<0x10000) return false;
    std::vector<uint8_t> buf((size_t)count*AE_STRIDE);
    if(!rd(entries,buf.data(),buf.size())) return false;
    for(uint32_t i=0;i<count;i++){
        const uint8_t* e=buf.data()+(size_t)i*AE_STRIDE;
        uint64_t rec,type;
        memcpy(&rec,e+AE_NAME,8); memcpy(&type,e+AE_TYPE,8);
        if(rec<0x10000 || !inMod(type)) return false;
        const std::string& nm=attrNameOf(rec);
        if(nm.empty()) return false;
        std::string* dst = (nm=="TeamID")        ? &out.teamId :
                           (nm=="EnvironmentID") ? &out.envId  : nullptr;
        if(!dst) continue;
        if(typeNameOf(type)!="string") continue;
        uint64_t len; memcpy(&len,e+AE_VALUE+0x10,8);
        if(len>256) continue;
        if(len<=15) dst->assign((const char*)(e+AE_VALUE),(size_t)len);
        else{
            uint64_t heap; memcpy(&heap,e+AE_VALUE,8);
            std::vector<char> t((size_t)len);
            if(heap>0x10000 && rd(heap,t.data(),(size_t)len)) dst->assign(t.data(),(size_t)len);
        }
    }
    out.ok=true;
    return true;
}

// TeamID / EnvironmentID of an instance. Prefers the kind==0x0F element and
// falls back to validating the other elements, so a changed tag degrades into
// a slower read instead of a silent wrong answer.
static bool attrRead(uint64_t inst, AttrSet& out){
    out=AttrSet();
    uint64_t vec=0;
    if(!rd(inst+I_STATEVEC,&vec,8) || vec<0x10000) return false;
    uint64_t be[2]={0,0};
    if(!rd(vec,be,16) || be[1]<be[0]) return false;
    uint64_t span=be[1]-be[0];
    if(!span || span%SV_ELEM || span>0x1000) return false;
    size_t n=(size_t)(span/SV_ELEM);
    std::vector<uint8_t> raw((size_t)span);
    if(!rd(be[0],raw.data(),(size_t)span)) return false;
    for(int pass=0;pass<2;pass++){
        for(size_t i=0;i<n;i++){
            uint64_t p; uint16_t kind;
            memcpy(&p,raw.data()+i*SV_ELEM,8);
            memcpy(&kind,raw.data()+i*SV_ELEM+8,2);
            if(p<0x10000) continue;
            bool isAttrTag = (kind==SV_KIND_ATTR);
            if(pass==0 ? !isAttrTag : isAttrTag) continue;
            if(attrParse(p,out)) return true;
        }
    }
    return false;
}

// ---------------------- world geometry, for the wall check ----------------------
// An oriented box per solid part. Out of process there is no raycast to borrow,
// so the only honest way to know whether a wall is in the way is to read the
// walls: every part's CFrame and Size, cached, then a segment/OBB test per
// target. Built incrementally on the main thread (a bounded number of nodes per
// frame) rather than on a worker, because the RTTI name memo is not thread safe
// and a data race there would be far worse than a few extra milliseconds.
struct OBB {
    Vec3  c;                 // centre
    Vec3  ax, ay, az;        // unit axes = COLUMNS of the row-major rotation
    float hx, hy, hz;        // half extents
    float r;                 // bounding-sphere radius, for the cheap reject
};

// One character, resolved once and then read cheaply every frame.
struct CharRef {
    uint64_t model=0, humanoid=0;
    uint64_t primHead=0, primRoot=0, primLFoot=0, primRFoot=0;
    // Rivals' REAL hit parts, picked up in the same child walk. The visual Head
    // mesh is not what the game hit-tests against, so the aimbot points here.
    uint64_t primAimHead=0, primAimBody=0;
    float    headHalfY=0.5f, footHalfY=0.5f;
    std::string name;
    unsigned long lastSeen=0;   // last tick this model came back from the Workspace walk
};

} // namespace rv

// Where the RBX::Instance sub-object sits inside a class, read out of that
// class's own RTTI base-class array. Factored out because the DataModel vftable
// can now arrive two ways: from the cached RVA, or from the full module sweep.
static uint32_t instanceOffsetFromCOL(const rv::COL& c){
    uint32_t instOff=0x1D0;
    rv::CHD h{};
    if(rv::mrd(rv::g_base+c.pCHD,&h,sizeof(h)) && h.numBase && h.numBase<128){
        std::vector<uint32_t> rvas(h.numBase);
        if(rv::mrd(rv::g_base+h.pBCA,rvas.data(),4ull*h.numBase)){
            for(uint32_t i=0;i<h.numBase;i++){
                rv::BCD b{};
                if(!rv::mrd(rv::g_base+rvas[i],&b,sizeof(b))) continue;
                char raw[320]{}, bn[320]{};
                if(!rv::mrd(rv::g_base+b.pTD+0x10,raw,sizeof(raw)-1)) continue;
                rv::demangle(raw,bn,sizeof(bn));
                if(strcmp(bn,"RBX::Instance")==0){ instOff=(uint32_t)b.mdisp; break; }
            }
        }
    }
    return instOff;
}

// How far from the local character another player can be and still count as
// sharing this arena. The concurrent matches sit thousands of studs apart, so
// anything in this range is genuinely in the same fight.
static const float ARENA_RADIUS = 1200.0f;   // studs, 3D
static const float ARENA_Y_BAND =  400.0f;   // studs, vertical - arenas are stacked in Y too
static int g_memSeen=0, g_memArena=0;        // characters read / kept after the arena filter
// Bootstrap cost split by stage, so 11.5 gets optimised against a measurement
// rather than a hunch - the last two CPU hunches in this project were both wrong.
static double g_bootAttach=0, g_bootVft=0, g_bootObj=0, g_bootTree=0, g_bootBytes=0;
static bool   g_bootCached=false;
static double nowSec(){
    LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart/(double)f.QuadPart;
}
static int g_memMates=0;                     // teammates dropped by the TeamID filter
static std::string g_myTeam, g_myEnv;        // local player's TeamID / EnvironmentID

// The published geometry the wall check reads, and the set being built.
static std::vector<rv::OBB> g_geom;
static std::set<uint64_t>   g_charModels;   // character subtrees the walk must not enter
static int   g_geomParts=0, g_geomNodes=0;
static float g_geomRadius=420.0f;           // studs around him; also scopes it to his arena
static const size_t GEOM_MAX = 9000;

// Names whose whole subtree is skipped. ViewModels is the important one: his own
// gun model sits a stud in front of the camera and would occlude every single
// ray. The rest are transient effects that would blink the check on and off.
static bool geomSkipName(const std::string& n){
    return n=="ViewModels" || n=="HurtEffect" || n=="TracerEffect" || n=="Camera" ||
           n=="Terrain"    || n=="_drop"      || n=="Debris";
}
// Diagnostics: when on, every character read is recorded with the reason it was
// (or was not) drawn, so a vanishing box can be traced instead of guessed at.
static bool g_trace=false;
struct TraceRow { std::string name; float hp, mhp; std::string team, env;
                  bool local; const char* drop; bool posOk; };
static std::vector<TraceRow> g_traceRows;
static int  g_traceCharsRead=0, g_traceNoPos=0, g_traceNoHum=0;
// rescan() drop tally: which STEP lost a character this pass.
static int g_rsKids=0, g_rsNotModel=0, g_rsChildFail=0, g_rsNoHum=0, g_rsNoPrim=0, g_rsKept=0;
// name -> outcome for every Workspace child this rescan looked at
static std::vector<std::pair<std::string,const char*>> g_rsOutcome;
static int g_rsRescued=0, g_rsExpired=0;
static int g_rsWsWalkFail=0, g_rsPasses=0;

// ---- rolling flight recorder -------------------------------------------
// Always on in the live overlay (it only costs anything when something CHANGES),
// so when a box blinks out he can press F7 AFTER seeing it and still have the
// history. Ring of the last N state changes; F7 writes them in order.
struct TraceEv { double t; char name[28]; float hp, mhp; char team[12], env[12];
                 char state[40]; };
static std::vector<TraceEv> g_ring;
static size_t g_ringPos=0, g_ringUsed=0;
static const size_t RING_CAP=4000;
static std::unordered_map<std::string,std::string> g_prevState;
static std::unordered_map<std::string,float>       g_prevHp;
// AUTO-CAPTURE: the failure signature is "an entity that was being DRAWN, is
// alive, and stopped being drawn". When that happens the recorder writes itself
// out unprompted, so the bug does not depend on anyone reaching for a key.
static std::set<std::string> g_prevDrawn;
static DWORD  g_lastAuto=0;
static int    g_autoCount=0;
static char   g_autoWhy[160]="";
static DWORD  g_autoFlash=0;

static void ringPush(double t, const TraceRow& r){
    if(g_ring.size()<RING_CAP) g_ring.resize(RING_CAP);
    TraceEv& e=g_ring[g_ringPos];
    e.t=t;
    snprintf(e.name,sizeof(e.name),"%s",r.name.c_str());
    e.hp=r.hp; e.mhp=r.mhp;
    auto hexify=[](const std::string& in, char* out, size_t n){
        if(in.empty()){ snprintf(out,n,"--"); return; }
        size_t w=0;
        for(size_t i=0;i<in.size() && w+3<n;i++) w+=snprintf(out+w,n-w,"%02X",(unsigned char)in[i]);
    };
    hexify(r.team,e.team,sizeof(e.team));
    hexify(r.env, e.env, sizeof(e.env));
    snprintf(e.state,sizeof(e.state),"%s",r.drop);
    g_ringPos=(g_ringPos+1)%RING_CAP;
    if(g_ringUsed<RING_CAP) g_ringUsed++;
}

static void autoCapture(double t, const char* name, const char* from, const char* to);
static void liveLog(const char* fmt, ...);   // the one live log, defined with the aimbot

// Record only what changed since the previous frame.
static void traceRecord(double t){
    std::set<std::string> seen, drawnNow;
    std::vector<std::string> trig;      // "name|from|to"
    for(size_t i=0;i<g_traceRows.size();i++){
        TraceRow& r=g_traceRows[i];
        seen.insert(r.name);
        if(strcmp(r.drop,"DRAWN")==0) drawnNow.insert(r.name);
        auto st=g_prevState.find(r.name);
        auto hpIt=g_prevHp.find(r.name);
        bool changed = (st==g_prevState.end()) || st->second!=r.drop ||
                       (hpIt!=g_prevHp.end() && fabsf(hpIt->second-r.hp)>0.01f);
        // the signature: was drawn, still alive, no longer drawn
        if(st!=g_prevState.end() && st->second=="DRAWN" &&
           strcmp(r.drop,"DRAWN")!=0 && r.hp>0.0f)
            trig.push_back(r.name+"|DRAWN|"+r.drop);
        if(!changed) continue;
        ringPush(t,r);
        g_prevState[r.name]=r.drop;
        g_prevHp[r.name]=r.hp;
    }
    // and the other way a box can go: the character left the list entirely
    for(std::set<std::string>::iterator it=g_prevDrawn.begin(); it!=g_prevDrawn.end(); ++it){
        if(seen.count(*it)) continue;
        float lastHp=0; auto h=g_prevHp.find(*it);
        if(h!=g_prevHp.end()) lastHp=h->second;
        if(lastHp<=0.0f) continue;                 // it just died; that is correct
        TraceRow r; r.name=*it; r.hp=lastHp; r.mhp=-1; r.local=false;
        r.drop="VANISHED-from-character-list"; r.posOk=false;
        ringPush(t,r);
        g_prevState[*it]="VANISHED-from-character-list";
        trig.push_back(*it+"|DRAWN|VANISHED-from-character-list");
    }
    g_prevDrawn.swap(drawnNow);
    for(size_t i=0;i<g_rsOutcome.size();i++){
        const std::string& nm=g_rsOutcome[i].first;
        std::string key="model:"+nm;
        auto st=g_prevState.find(key);
        if(st!=g_prevState.end() && st->second==g_rsOutcome[i].second) continue;
        TraceRow r; r.name=nm; r.hp=-1; r.mhp=-1; r.local=false;
        r.drop=g_rsOutcome[i].second; r.posOk=false;
        ringPush(t,r);
        g_prevState[key]=g_rsOutcome[i].second;
    }
    g_rsOutcome.clear();
    for(size_t i=0;i<trig.size();i++){
        size_t a=trig[i].find('|'), b=trig[i].rfind('|');
        if(a==std::string::npos||b==std::string::npos||b<=a) continue;
        autoCapture(t, trig[i].substr(0,a).c_str(),
                       trig[i].substr(a+1,b-a-1).c_str(),
                       trig[i].substr(b+1).c_str());
    }
}

static void traceDump(const char* path, const char* why);

// One dump per 20 s so a busy round cannot spam the disk; the file always holds
// the seconds leading up to the most recent occurrence.
static void autoCapture(double t, const char* name, const char* from, const char* to){
    // The one-line timeline goes in the single live log with everything else;
    // the full ring dump goes to overlay_log.txt. Two files, not six - the old
    // one-file-per-reason split existed to stop benign churn overwriting a real
    // capture, and a rate limit does that just as well without the litter.
    liveLog("DROP   %-18s %s -> %s  (alive)", name, from, to);

    DWORD now=GetTickCount();
    if(g_lastAuto && now-g_lastAuto < 20000) return;
    g_lastAuto=now; g_autoCount++;
    snprintf(g_autoWhy,sizeof(g_autoWhy),
             "AUTO #%d at %.2fs - %s went %s -> %s while ALIVE",
             g_autoCount,t,name,from,to);
    traceDump("overlay_log.txt",g_autoWhy);
    g_autoFlash=now;
}

static void traceDump(const char* path, const char* why){
    FILE* d=fopen(path,"w");
    if(!d) return;
    fprintf(d,"RIVALS EXTERNAL - flight recorder (%s)\n", why);
    {
        char lt[12]="--", le[12]="--"; size_t w=0;
        if(!g_myTeam.empty()){ w=0; for(size_t i=0;i<g_myTeam.size()&&w+3<sizeof(lt);i++) w+=snprintf(lt+w,sizeof(lt)-w,"%02X",(unsigned char)g_myTeam[i]); }
        if(!g_myEnv.empty()){  w=0; for(size_t i=0;i<g_myEnv.size() &&w+3<sizeof(le);i++) w+=snprintf(le+w,sizeof(le)-w,"%02X",(unsigned char)g_myEnv[i]); }
        fprintf(d,"last %llu state changes; ws walk %d passes / %d failed\n",
                (unsigned long long)g_ringUsed, g_rsPasses, g_rsWsWalkFail);
        fprintf(d,"local TeamID %s  EnvironmentID %s   options: hideTeam=%d envFilter=%d esp=%d\n\n",
                lt, le, (int)g_opt.hideTeam, (int)g_opt.envFilter, (int)g_opt.esp);
    }
    size_t start = (g_ringUsed<RING_CAP) ? 0 : g_ringPos;
    for(size_t k=0;k<g_ringUsed;k++){
        const TraceEv& e=g_ring[(start+k)%RING_CAP];
        if(e.hp<0) fprintf(d,"%9.2fs  MODEL %-22s %s\n", e.t, e.name, e.state);
        else       fprintf(d,"%9.2fs  %-22s hp %7.2f/%-7.2f T%-6s E%-6s %s\n",
                           e.t, e.name, e.hp, e.mhp, e.team, e.env, e.state);
    }
    fclose(d);
}

// ---------------------------------------------------------------------------
// Both live with the aimbot further down, but poll() computes the wall-check
// answer once per frame now so three consumers can share it.
static bool occluded(Vec3 from, Vec3 to);
static Vec3 aimPointFor(const Entity& e, int part);
// The live Camera / DataModel / Workspace, published at bootstrap so the RE
// workshop can use them without standing up a MemSource of its own.
static uint64_t g_camAddr = 0, g_dmAddr = 0, g_wsAddr = 0;

struct MemSource {
    bool     ready=false;
    uint64_t dm=0, ws=0, cam=0;
    uint64_t dmObj=0, dmVft=0;      // kept so a rebuilt DataModel can be spotted
    uint64_t players=0;             // RBX::Players service
    uint64_t localPlayer=0, localChar=0;
    std::string myTeam, myEnv;
    struct PInfo { std::string team, env; };
    std::unordered_map<uint64_t,PInfo> byModel;   // character Model -> that player's team
    std::unordered_map<std::string,PInfo> byName; // and by player name, for the window after a
                                                  // respawn where Character points at a new Model
    std::vector<rv::CharRef> chars;
    DWORD    lastScan=0, lastTry=0, lastFull=0, lastTeam=0;
    uint64_t wsSig=0;
    int      failStreak=0;

    // Find the DataModel by RTTI class name, then Workspace and Camera under it.
    bool bootstrap(){
        double t0=nowSec();
        if(!rv::attach()) return false;
        double t1=nowSec();

        uint64_t dmVft_=0; uint32_t instOff=0x1D0;
        // Cached RVA first - but it is VERIFIED, never trusted. Roblox shipped an
        // update between two runs of this project and the RVA moved
        // (0x69795D8 -> 0x6B440D8); nothing broke precisely because nothing was
        // hardcoded, and this cache must not regress that. A miss just costs the
        // full sweep, which is what used to happen every single launch.
        if(g_dmVftRva && (uint64_t)g_dmVftRva+16 < rv::g_size){
            uint64_t vft=rv::g_base+g_dmVftRva;
            char nm[320]; rv::COL c{};
            if(rv::rttiName(vft,nm,sizeof(nm),&c) && c.offset==0 &&
               strcmp(nm,"RBX::DataModel")==0){
                dmVft_=vft; instOff=instanceOffsetFromCOL(c); g_bootCached=true;
            }
        }
        if(!dmVft_){
            rv::loadImage();
            for(size_t o=0; o+16<=rv::g_img.size(); o+=8){
                uint64_t v; memcpy(&v, rv::g_img.data()+o, 8);
                if(!rv::inMod(v)) continue;
                uint64_t vft = rv::g_base + o + 8;
                char nm[320]; rv::COL c{};
                if(!rv::rttiName(vft,nm,sizeof(nm),&c)) continue;
                if(c.offset!=0 || strcmp(nm,"RBX::DataModel")!=0) continue;
                dmVft_=vft; instOff=instanceOffsetFromCOL(c);
                break;
            }
        }
        double t2=nowSec();
        if(!dmVft_) return false;
        {   // remember where it was so the next launch skips the 144 MB sweep
            uint32_t rva=(uint32_t)(dmVft_-rv::g_base);
            if(rva!=g_dmVftRva){ g_dmVftRva=rva; cfgSave(); }
        }

        // Exactly one live object carries that vftable. Finding it is the whole
        // remaining cost of bootstrap - a couple of GB of committed memory read
        // through ReadProcessMemory - so do it in two passes. A C++ object lives
        // in private read/write heap, which is a fraction of what is committed
        // (the rest is mapped files, textures and read-only image data), and in
        // practice pass 1 always wins. Pass 2 exists so that if it ever does not,
        // bootstrap still succeeds instead of silently failing.
        // MEASURED: this hunt was the ENTIRE bootstrap cost - 7.08 s of a 7.09 s
        // startup, with the RTTI sweep and the tree walk rounding to zero. Two
        // things were wrong with it. It called buf.resize(region.size) per region,
        // which value-initialises, i.e. it memset roughly a gigabyte on top of
        // reading it; and it did every ReadProcessMemory serially, which tops out
        // near 170 MB/s because the cost is the kernel's cross-process copy, not
        // our search. Now: one reused chunk buffer per thread, and the regions are
        // handed out to a few threads from an atomic cursor. Private read/write
        // regions go first because a C++ object lives in the heap; the rest are
        // still covered, so nothing can be missed if that assumption ever breaks.
        uint64_t obj=0;
        {
            const size_t CHUNK = 4u<<20;
            std::vector<rv::Region> all=rv::regions(), order;
            order.reserve(all.size());
            for(int pass=0; pass<2; pass++)
                for(auto& r : all){
                    if(r.base>=rv::g_base && r.base<rv::g_base+rv::g_size) continue;
                    bool heap = (r.type==MEM_PRIVATE) &&
                                ((r.prot&0xFF)==PAGE_READWRITE || (r.prot&0xFF)==PAGE_WRITECOPY);
                    if(pass==0 ? !heap : heap) continue;
                    order.push_back(r);
                }
            std::atomic<size_t>   cursor(0);
            std::atomic<uint64_t> found(0), bytes(0);
            unsigned nT=std::thread::hardware_concurrency();
            if(!nT) nT=4; if(nT>6) nT=6;
            if((size_t)nT>order.size()) nT=(unsigned)(order.size()?order.size():1);
            auto worker=[&](){
                std::vector<uint8_t> buf(CHUNK);
                uint64_t mine=0;
                for(;;){
                    size_t i=cursor.fetch_add(1);
                    if(i>=order.size() || found.load(std::memory_order_relaxed)) break;
                    const rv::Region& r=order[i];
                    for(size_t off=0; off<r.size; off+=CHUNK){
                        if(found.load(std::memory_order_relaxed)) break;
                        size_t n=r.size-off; if(n>CHUNK) n=CHUNK;
                        SIZE_T got=0;
                        if(!ReadProcessMemory(rv::g_h,(LPCVOID)(r.base+off),buf.data(),n,&got)||got<8) continue;
                        mine+=got;
                        const uint64_t* q=(const uint64_t*)buf.data();
                        size_t nq=got/8;
                        for(size_t j=0;j<nq;j++)
                            if(q[j]==dmVft_){ found.store(r.base+off+j*8); break; }
                    }
                }
                bytes.fetch_add(mine);
            };
            std::vector<std::thread> th;
            for(unsigned t=1;t<nT;t++) th.emplace_back(worker);
            worker();
            for(auto& t : th) t.join();
            obj=found.load();
            g_bootBytes=(double)bytes.load();
        }
        double t3=nowSec();
        if(!obj) return false;
        dm = obj + instOff;

        ws  = rv::childByClass(dm, "RBX::Workspace");
        if(!ws) return false;
        cam = rv::childByClass(ws, "RBX::Camera");
        if(!cam) return false;
        g_camAddr = cam;      // the RE workshop reads it straight from here
        g_dmAddr  = dm;       // and the RE workshop's `scan dm` starts from these
        g_wsAddr  = ws;
        // Not fatal: without Players the overlay still runs, it just falls back
        // to the Camera.Focus heuristic and the proximity arena filter.
        players = rv::childByClass(dm, "RBX::Players");

        g_bootAttach=t1-t0; g_bootVft=t2-t1; g_bootObj=t3-t2; g_bootTree=nowSec()-t3;
        dmObj=obj; dmVft=dmVft_;
        // The module snapshot was only needed to find the DataModel vftable.
        // Drop it - 144 MB resident for nothing otherwise; RTTI lookups after
        // this point fall back to ReadProcessMemory and are memoised anyway.
        std::vector<uint8_t>().swap(rv::g_img);
        chars.clear(); byModel.clear(); localPlayer=0; localChar=0;
        myTeam.clear(); myEnv.clear();
        lastScan=0; lastFull=0; lastTeam=0; wsSig=0; failStreak=0;
        ready=true;
        return true;
    }

    // Re-resolve which Workspace children are characters (a Model with a Humanoid).
    // A lobby<->match teleport rebuilds the DataModel. The old addresses often stay
    // READABLE afterwards, just meaningless, so a failed read is not a reliable
    // signal - check the DataModel object still carries the DataModel vftable.
    bool stillValid(){
        uint64_t v=0;
        if(!rv::rd(dmObj,&v,8) || v!=dmVft) return false;
        return rv::classOf(ws)=="RBX::Workspace";
    }

    // Cheap change-detector: hash the Workspace children pointer array. A full
    // rescan only earns its cost when that set actually changed (someone joined,
    // left, or respawned into a fresh Model).
    uint64_t wsSignature(){
        uint64_t vec=0;
        if(!rv::rd(ws+rv::I_CHILDREN,&vec,8) || vec<0x10000) return 0;
        uint64_t be[2]={0,0};
        if(!rv::rd(vec,be,16) || be[1]<be[0]) return 0;
        uint64_t span=be[1]-be[0];
        if(span%16 || span>16ull*100000) return 0;
        size_t cnt=(size_t)(span/16);
        if(!cnt) return 1;
        std::vector<uint64_t> raw(cnt*2);
        if(!rv::rd(be[0],raw.data(),cnt*16)) return 0;
        uint64_t hsh=1469598103934665603ull;                   // FNV-1a over the _Ptr slots
        for(size_t i=0;i<cnt;i++){ hsh^=raw[i*2]; hsh*=1099511628211ull; }
        return hsh?hsh:1;
    }

    // Who is who: LocalPlayer by pointer identity (not a camera heuristic, so it
    // is correct in first person, third person and while spectating), and every
    // player's TeamID / EnvironmentID keyed by the character Model the ESP sees.
    void refreshPlayers(){
        byModel.clear(); byName.clear(); localChar=0; myTeam.clear(); myEnv.clear();
        if(!players) return;
        uint64_t lp=0;
        if(rv::rd(players+rv::PS_LOCALPLAYER,&lp,8) && lp>0x10000 &&
           rv::classOf(lp)=="RBX::Player"){
            localPlayer=lp;
            uint64_t ch=0;
            if(rv::rd(lp+rv::PL_CHARACTER,&ch,8) && ch>0x10000) localChar=ch;
            rv::AttrSet a;
            if(rv::attrRead(lp,a)){ myTeam=a.teamId; myEnv=a.envId; }
        }
        std::vector<uint64_t> kids;
        if(!rv::childrenOf(players,kids)) return;
        for(size_t i=0;i<kids.size();i++){
            if(rv::classOf(kids[i])!="RBX::Player") continue;
            uint64_t ch=0;
            if(!rv::rd(kids[i]+rv::PL_CHARACTER,&ch,8) || ch<0x10000) continue;
            PInfo pi;
            rv::AttrSet a;
            if(rv::attrRead(kids[i],a)){ pi.team=a.teamId; pi.env=a.envId; }
            byModel[ch]=pi;
            std::string pn;
            if(rv::nameOf(kids[i],pn) && !pn.empty()) byName[pn]=pi;
        }
        g_myTeam=myTeam; g_myEnv=myEnv;
    }

    // A character we already resolved is still ours if its Model is still
    // parented to Workspace, still a Model, and still carries the same name.
    // Three cheap reads, and they are what let a character survive a pass of the
    // Workspace walk that did not list it.
    // Is a character we already resolved still worth drawing?
    //
    // Deliberately does NOT ask whether the Model is still a child of Workspace.
    // Measured: during combat a character's Parent field transiently reads as
    // something other than Workspace and the model drops out of the child vector
    // for ~200 ms, while the executor oracle - watching the same characters at
    // Heartbeat rate - saw NO parent change and NO model swap at all. Whatever
    // the engine is doing in there (a reparent we only catch mid-flight, or a
    // child vector being shifted under our read), tree membership is not a
    // trustworthy per-frame signal and using it as one is what made boxes blink
    // out whenever anyone nearby took damage.
    //
    // What IS trustworthy is the object the CharRef already points at: if the
    // Humanoid still reports a sane health and a Primitive still reports a sane
    // position, that character is alive and its cached pointers are good. The
    // name check guards against the model being freed and its memory reused, and
    // CARRY_MS caps how long anything can survive without the walk seeing it.
    bool stillOurs(const rv::CharRef& c, const char** why=nullptr){
        // The model must still be a DESCENDANT of Workspace - not necessarily a
        // direct child of it.
        //
        // Measured, and this is the whole bug: taking damage moves a Rivals
        // character OUT of Workspace's direct children for about a second.
        //   5.97s  Bubbalicious106  hp 95/100   DRAWN
        //   6.09s  MODEL Bubbalicious106  reparented   -> box gone
        //   7.22s  Bubbalicious106  hp 100/100  DRAWN   -> box back
        // rescan() only walks DIRECT children of Workspace, so the character
        // disappeared from the list, and an equality test against `ws` then
        // refused to carry it. Walking the parent chain keeps it: still under
        // Workspace = still ours. An orphan (chain hits 0) is still rejected, so
        // respawn ghosts cannot come back.
        uint64_t par=c.model, root=0;
        int hops=0;
        for(; hops<10; ++hops){
            uint64_t nxt=0;
            if(!rv::rd(par+rv::I_PARENT,&nxt,8)) break;
            if(nxt==ws){ root=ws; break; }
            if(nxt<0x10000){ root=0; break; }   // detached: orphaned
            par=nxt;
        }
        if(root!=ws){
            if(why)*why = (hops>=10) ? "parent-chain-too-deep"
                        : (par<0x10000 ? "orphaned(respawn)" : "detached-from-workspace");
            return false;
        }
        float hp=0, mhp=0;
        if(!rv::rd(c.humanoid+rv::H_HEALTH,&hp,4)){ if(why)*why="humanoid-unreadable"; return false; }
        if(!rv::rd(c.humanoid+rv::H_MAXHEALTH,&mhp,4)){ if(why)*why="maxhealth-unreadable"; return false; }
        if(!(hp>=0.0f && hp<100000.0f) || !(mhp>0.0f && mhp<100000.0f)){
            if(why)*why="health-not-sane"; return false;
        }
        Vec3 v;
        if(!rv::primPos(c.primHead,v) && !rv::primPos(c.primRoot,v)){
            if(why)*why="no-position"; return false;
        }
        if(!(fabsf(v.x)<1e6f && fabsf(v.y)<1e6f && fabsf(v.z)<1e6f)){
            if(why)*why="position-not-sane"; return false;
        }
        rv::Hdr h=rv::hdrOf(c.model);
        std::string n;
        if(!h.ok || !rv::nameFromRec(h.nameRec,n) || n!=c.name){
            if(why)*why="model-memory-reused"; return false;
        }
        return true;
    }

    // INCREMENTAL, not destructive. Measured cause of the vanishing-ESP bug:
    // during combat a character Model briefly stops being listed among the
    // Workspace children (Rivals reparents it on damage/respawn, and the child
    // vector is being mutated constantly by hit effects). The old rescan cleared
    // `chars` first and rebuilt, so any character the walk missed for one pass
    // disappeared from the ESP for 150 ms+ - which is exactly the flicker.
    // Now the walk only ADDS, and anything it missed has to fail an explicit
    // liveness check before it is dropped. A character that keeps failing to
    // show up for CARRY_MS is retired anyway, so nothing can ghost forever.
    // Generous, because the liveness check above is now a real proof of
    // membership rather than a guess: anything that genuinely leaves the tree
    // fails it on the very next pass regardless of this number.
    static const unsigned long CARRY_MS = 4000;   // only a bridge now; discovery finds them
    void rescan(){
        if(!stillValid()){ chars.clear(); ready=false; return; }
        std::vector<uint64_t> kids;
        g_rsPasses++;
        if(!rv::childrenOf(ws,kids)){          // keep what we have; do NOT clear
            g_rsWsWalkFail++;
            return;
        }
        std::vector<rv::CharRef> fresh;
        unsigned long nowTk=GetTickCount();
        g_rsKids=(int)kids.size();
        g_rsNotModel=g_rsChildFail=g_rsNoHum=g_rsNoPrim=g_rsKept=0;
        if(g_trace) g_rsOutcome.clear();
        // Character models are NOT always direct children of Workspace. Measured:
        // when a Rivals player takes damage the engine reparents their character
        // under a "HurtEffect" Model for a second or more, and a depth-1 walk
        // simply loses them - which is what made boxes wink out on every hit.
        // So the walk descends one extra level: Workspace -> Model -> Model.
        // It costs nothing, because the children of every Model are already being
        // read to look for a Humanoid.
        std::vector<uint64_t> sub;
        std::vector<int> depth(kids.size(),0);
        for(size_t i=0;i<kids.size();i++){
            uint64_t m=kids[i];
            int d=depth[i];
            rv::Hdr mh=rv::hdrOf(m);
            if(!mh.ok || rv::classOfVft(mh.vft)!="RBX::ModelInstance"){
                g_rsNotModel++;
                if(g_trace){ std::string n; if(rv::nameFromRec(mh.nameRec,n)) g_rsOutcome.push_back({n,"not-a-model"}); }
                continue;
            }
            if(!rv::childrenFromVec(mh.childVec,sub)){
                g_rsChildFail++;
                if(g_trace){ std::string n; if(rv::nameFromRec(mh.nameRec,n)) g_rsOutcome.push_back({n,"children-read-FAILED"}); }
                continue;
            }

            // ONE pass over the model's children picking up everything at once,
            // instead of five separate walks looking for one name each.
            uint64_t hum=0, head=0, root=0, lf=0, rf=0;
            uint64_t hbHead=0, hbHead2=0, hbBody=0, hbBody2=0;
            for(size_t k=0;k<sub.size();k++){
                rv::Hdr kh=rv::hdrOf(sub[k]);
                if(!kh.ok) continue;
                const std::string& cl=rv::classOfVft(kh.vft);
                if(!hum && cl=="RBX::Humanoid"){ hum=sub[k]; continue; }
                if(cl!="RBX::Part" && cl!="RBX::MeshPart") continue;
                std::string kn;
                if(!rv::nameFromRec(kh.nameRec,kn)) continue;
                if     (kn=="Head")               head  =sub[k];
                else if(kn=="HumanoidRootPart")   root  =sub[k];
                else if(kn=="LeftFoot")           lf    =sub[k];
                else if(kn=="RightFoot")          rf    =sub[k];
                // Aim targets, in preference order. HitboxHead is the one Rivals
                // actually registers head shots on; the *Small variants and
                // PhysicalHitboxHead are the fallbacks when it is absent.
                else if(kn=="HitboxHead")         hbHead =sub[k];
                else if(kn=="PhysicalHitboxHead") hbHead2=sub[k];
                else if(kn=="HitboxHeadSmall"){ if(!hbHead2) hbHead2=sub[k]; }
                else if(kn=="HitboxBody")         hbBody =sub[k];
                else if(kn=="HitboxBodySmall")    hbBody2=sub[k];
            }
            if(!hum){
                // No Humanoid here - but a container like HurtEffect holds the real
                // character one level down, so queue its Model children.
                if(d<1){
                    for(size_t k=0;k<sub.size();k++){
                        rv::Hdr kh=rv::hdrOf(sub[k]);
                        if(!kh.ok || rv::classOfVft(kh.vft)!="RBX::ModelInstance") continue;
                        kids.push_back(sub[k]); depth.push_back(d+1);
                    }
                }
                g_rsNoHum++;
                if(g_trace){ std::string n; if(rv::nameFromRec(mh.nameRec,n)) g_rsOutcome.push_back({n,"no-humanoid"}); }
                continue;
            }

            rv::CharRef c; c.model=m; c.humanoid=hum;
            if(!rv::nameFromRec(mh.nameRec,c.name)) continue;
            c.primHead =rv::primOf(head);
            c.primRoot =rv::primOf(root);
            c.primLFoot=rv::primOf(lf);
            c.primRFoot=rv::primOf(rf);
            c.primAimHead=rv::primOf(hbHead ? hbHead : hbHead2);
            c.primAimBody=rv::primOf(hbBody ? hbBody : hbBody2);
            if(!c.primHead && !c.primRoot){
                g_rsNoPrim++;
                if(g_trace) g_rsOutcome.push_back({c.name,"no-primitive"});
                continue;
            }
            g_rsKept++;
            if(g_trace) g_rsOutcome.push_back({c.name,"KEPT"});
            Vec3 sz;
            if(rv::primSize(c.primHead,sz))  c.headHalfY=sz.y*0.5f;
            if(rv::primSize(c.primLFoot,sz)) c.footHalfY=sz.y*0.5f;
            c.lastSeen=nowTk;
            fresh.push_back(c);
        }

        // Carry over anyone this pass of the walk did not list but who is still
        // demonstrably alive in the tree.
        g_rsRescued=g_rsExpired=0;
        for(size_t i=0;i<chars.size();i++){
            const rv::CharRef& old=chars[i];
            bool found=false;
            for(size_t j=0;j<fresh.size();j++)
                if(fresh[j].model==old.model){ found=true; break; }
            if(found) continue;
            if(nowTk-old.lastSeen > CARRY_MS){
                g_rsExpired++;
                if(g_trace) g_rsOutcome.push_back({old.name,"EXPIRED-after-carry"});
                continue;
            }
            const char* why="?";
            if(!stillOurs(old,&why)){
                if(g_trace) g_rsOutcome.push_back({old.name,why});
                continue;
            }
            g_rsRescued++;
            if(g_trace){
                uint64_t par=0; rv::rd(old.model+rv::I_PARENT,&par,8);
                if(par==ws) g_rsOutcome.push_back({old.name,"CARRIED(missed-by-walk)"});
                else{
                    static char buf[160];
                    std::string pn; rv::nameOf(par,pn);
                    snprintf(buf,sizeof(buf),"CARRIED(under %s [%s])",
                             pn.empty()?"?":pn.c_str(), rv::classOf(par).c_str());
                    g_rsOutcome.push_back({old.name,buf});
                }
            }
            fresh.push_back(old);
        }
        chars.swap(fresh);
        g_charModels.clear();
        for(size_t i=0;i<chars.size();i++) g_charModels.insert(chars[i].model);
    }

    // ---- incremental geometry scan for the wall check ----
    // A breadth-first walk of Workspace that processes a bounded number of nodes
    // per frame, so a 20,000-node map costs a few tenths of a millisecond each
    // frame for a fraction of a second instead of one enormous hitch.
    std::vector<uint64_t> gStack;
    std::vector<rv::OBB>  gOut;
    bool  gRunning=false;
    DWORD gLast=0;
    Vec3  gOrigin{};

    void geomStep(const Vec3& origin){
        DWORD now=GetTickCount();
        if(!gRunning){
            Vec3 d=sub(origin,gOrigin);
            bool moved = dot(d,d) > 150.0f*150.0f;      // new arena / teleport
            if(!(gLast==0 || moved || now-gLast>20000)) return;
            gStack.clear(); gOut.clear();
            gStack.push_back(ws);
            gOrigin=origin; gRunning=true; g_geomNodes=0;
        }
        const int BUDGET=700;
        int done=0;
        std::vector<uint64_t> kids;
        while(!gStack.empty() && done<BUDGET){
            uint64_t inst=gStack.back(); gStack.pop_back();
            done++; g_geomNodes++;
            rv::Hdr h=rv::hdrOf(inst);
            if(!h.ok) continue;
            const std::string& cl=rv::classOfVft(h.vft);
            std::string nm;
            rv::nameFromRec(h.nameRec,nm);
            if(geomSkipName(nm)) continue;
            // Never walk into a player: their rig is not cover, and their own
            // torso would occlude their head.
            if(g_charModels.count(inst)) continue;

            if(cl=="RBX::Part" || cl=="RBX::MeshPart" || cl=="RBX::TrussPart" ||
               cl=="RBX::WedgePart" || cl=="RBX::CornerWedgePart" ||
               cl=="RBX::PartOperation" || cl=="RBX::NegateOperation"){
                uint64_t prim=rv::primOf(inst);
                float rot[9]; Vec3 pos, sz;
                if(prim && rv::primCFrame(prim,rot,pos) && rv::primSize(prim,sz)){
                    Vec3 dd=sub(pos,origin);
                    if(dot(dd,dd) <= g_geomRadius*g_geomRadius){
                        // Only things big enough to actually be cover. Rivals'
                        // maps are full of small props and dropped weapons; a
                        // 1-stud crate is not what makes a shot impossible, and
                        // including them makes the check twitchy.
                        float a=sz.x, b=sz.y, cc=sz.z;
                        float m1=a>b?a:b; if(cc>m1) m1=cc;             // largest
                        float m3=a<b?a:b; if(cc<m3) m3=cc;             // smallest
                        float m2=a+b+cc-m1-m3;                          // middle
                        if(m1>=3.0f && m2>=2.0f && gOut.size()<GEOM_MAX){
                            rv::OBB o;
                            o.c=pos;
                            o.ax=norm(Vec3{rot[0],rot[3],rot[6]});
                            o.ay=norm(Vec3{rot[1],rot[4],rot[7]});
                            o.az=norm(Vec3{rot[2],rot[5],rot[8]});
                            // A box with a degenerate basis reports a hit on
                            // EVERY ray (all three slab tests degenerate to
                            // "no constraint"), so one bad read would silently
                            // disable the aimbot everywhere. Drop it instead.
                            if(fabsf(dot(o.ax,o.ax)-1.0f)<0.01f &&
                               fabsf(dot(o.ay,o.ay)-1.0f)<0.01f &&
                               fabsf(dot(o.az,o.az)-1.0f)<0.01f){
                                o.hx=sz.x*0.5f; o.hy=sz.y*0.5f; o.hz=sz.z*0.5f;
                                o.r=sqrtf(o.hx*o.hx+o.hy*o.hy+o.hz*o.hz);
                                gOut.push_back(o);
                            }
                        }
                    }
                }
            }
            if(rv::childrenFromVec(h.childVec,kids))
                for(size_t i=0;i<kids.size();i++) gStack.push_back(kids[i]);
        }
        if(gStack.empty()){
            g_geom.swap(gOut); gOut.clear();
            g_geomParts=(int)g_geom.size();
            gRunning=false; gLast=now;
        }
    }

    bool poll(Frame& out) {
        DWORD now=GetTickCount();
        if(!ready){
            if(now-lastTry < 1000) return false;    // don't hammer while the game starts
            lastTry=now;
            if(!bootstrap()) return false;
        }

        // ---- camera ----
        float rot[9], pos[3], focus[3], fovRad=0;
        if(!rv::rd(cam+rv::C_ROT,rot,36) || !rv::rd(cam+rv::C_POS,pos,12) ||
           !rv::rd(cam+rv::C_FOV,&fovRad,4)){
            if(++failStreak>30){ ready=false; failStreak=0; }   // rejoin / shutdown
            return false;
        }
        rv::rd(cam+rv::C_FOCUSPOS,focus,12);
        // Garbage-in guard: a stale camera address usually still reads, so check
        // the values are actually a camera - FOV in range and a unit basis.
        float rlen = rot[0]*rot[0]+rot[3]*rot[3]+rot[6]*rot[6];
        if(!(fovRad>0.05f && fovRad<3.10f) || rlen<0.9f || rlen>1.1f){
            if(++failStreak>30){ ready=false; failStreak=0; }
            return false;
        }
        failStreak=0;

        Frame fr;
        // Roblox CFrame rotation is row-major R00..R22; the basis vectors are its
        // COLUMNS, and LookVector is the negated third column.
        fr.cam.pos   = { pos[0], pos[1], pos[2] };
        fr.cam.right = { rot[0], rot[3], rot[6] };
        fr.cam.up    = { rot[1], rot[4], rot[7] };
        fr.cam.look  = { -rot[2], -rot[5], -rot[8] };
        fr.cam.fovDeg= fovRad * (180.0f/3.14159265f);
        int cx,cy,cw,ch;
        if(robloxClient(cx,cy,cw,ch) && cw>0 && ch>0){ fr.cam.w=cw; fr.cam.h=ch; }
        else { fr.cam.w=1920; fr.cam.h=1080; }
        fr.cam.insetX=0; fr.cam.insetY=0;      // viewport == client area, no inset

        // ---- characters ----
        // Rescan only when the Workspace child set changed, or every 5s as a
        // safety net for parts swapped inside an unchanged Model.
        if(now-lastScan > 150){
            uint64_t sig=wsSignature();
            if(sig!=wsSig || now-lastFull > 5000 || chars.empty()){
                rescan(); wsSig=sig; lastFull=now;
            }
            lastScan=now;
        }
        // Team state changes without the Workspace child set changing (a round
        // starts and everyone gets a TeamID), so it gets its own 2 Hz refresh.
        if(now-lastTeam > 500 || byModel.empty()){ refreshPlayers(); lastTeam=now; }

        Vec3 focusV={focus[0],focus[1],focus[2]};
        int  localIdx=-1; float localBest=16.0f;   // studs^2, in the XZ plane

        std::vector<Entity> ents; ents.reserve(chars.size());
        std::vector<int>    srcIdx; srcIdx.reserve(chars.size());

        for(size_t i=0;i<chars.size();i++){
            rv::CharRef& c=chars[i];
            // Health (+0x190) and MaxHealth (+0x1A8) sit 0x18 apart, so ONE
            // 28-byte read fetches both. Every ReadProcessMemory is a syscall
            // plus a cross-process copy whose cost is dominated by the crossing,
            // not the size - halving the count here halves this line item.
            float blk[7]={0};
            if(!rv::rd(c.humanoid+rv::H_HEALTH,blk,28)){
                if(g_trace){ g_traceNoHum++; g_traceRows.push_back({c.name,-1,-1,"","",false,"humanoid-read-failed",false}); }
                continue;
            }
            float hp[1]={blk[0]}, mhp[1]={blk[(rv::H_MAXHEALTH-rv::H_HEALTH)/4]};

            Vec3 head{}, root{}, lf{}, rf{};
            bool hasHead=rv::primPos(c.primHead,head);
            bool hasRoot=rv::primPos(c.primRoot,root);
            if(!hasHead && !hasRoot){
                if(g_trace){ g_traceNoPos++; g_traceRows.push_back({c.name,hp[0],mhp[0],"","",false,"no-position",false}); }
                continue;
            }
            if(!hasHead) head=root;
            if(!hasRoot) root=head;

            // Focus tracks the local humanoid in Roblox's default camera. Rivals
            // uses a custom FPS camera in a match where it does not, which is why
            // LocalPlayer.Character is the real answer - this stays only as the
            // fallback for when the Players chain is unavailable.
            float dx=root.x-focusV.x, dz=root.z-focusV.z;
            float d2=dx*dx+dz*dz;
            if(d2<localBest){ localBest=d2; localIdx=(int)ents.size(); }

            // The feet are only ever used to place the BOTTOM of the ESP box, so
            // two reads per player per frame are skipped outright when nothing
            // is drawing boxes.
            float botY;
            bool needFeet = g_opt.esp && (g_opt.box || g_opt.health || g_opt.snapline);
            bool hl=false, hr=false;
            if(needFeet){ hl=rv::primPos(c.primLFoot,lf); hr=rv::primPos(c.primRFoot,rf); }
            if(hl||hr){
                float a = hl ? lf.y-c.footHalfY : 1e9f;
                float b = hr ? rf.y-c.footHalfY : 1e9f;
                botY = a<b?a:b;
            } else botY = root.y-3.0f;          // HumanoidRootPart is body CENTRE, not feet

            Entity e;
            e.name=c.name;
            e.id=c.model;
            e.health=hp[0]; e.maxHealth=mhp[0]>0?mhp[0]:100.0f;
            e.head={ head.x, head.y+c.headHalfY, head.z };
            e.root={ root.x, botY, root.z };
            e.width=3.2f;
            e.enemy=true;
            // Aim points. Fall back to the rig itself when a character has no
            // hitbox parts (they exist on players, not on every Model with a
            // Humanoid), so the aimbot never loses a target over a missing part.
            Vec3 ap;
            bool needAim = g_opt.aim;
            if(needAim && rv::primPos(c.primAimHead,ap)){ e.aimHead=ap; e.hasAimHead=true; }
            else if(hasHead){ e.aimHead=head; e.hasAimHead=true; }
            if(needAim && rv::primPos(c.primAimBody,ap)){ e.aimBody=ap; e.hasAimBody=true; }
            else if(hasRoot){ e.aimBody=root; e.hasAimBody=true; }
            // Model pointer first; player name as the fallback, because a respawn
            // swaps the character Model and the pointer key goes stale until the
            // next 2 Hz sweep. Workspace character models are named after the player.
            auto pit=byModel.find(c.model);
            if(pit!=byModel.end()){ e.team=pit->second.team; e.env=pit->second.env; }
            else{
                auto nit=byName.find(c.name);
                if(nit!=byName.end()){ e.team=nit->second.team; e.env=nit->second.env; }
            }
            e.local = (localChar && c.model==localChar);
            // The head-near-camera fallback is ONLY safe when we do not know who we
            // are. Measured in a real duel: enemies were being flagged local and
            // dropped mid-fight ("DRAWN -> local") because in a first-person shooter
            // an enemy's head really does come within a few studs of your camera -
            // which is exactly when you are trading damage. So it now runs only when
            // the Players chain gave us no character at all.
            if(!e.local && !localChar){
                Vec3 hd=sub(e.head,fr.cam.pos);
                if(dot(hd,hd) < 9.0f) e.local=true;
            }
            ents.push_back(e);
            srcIdx.push_back((int)i);
        }

        // If the Players chain gave us nothing, fall back to the Focus heuristic.
        bool haveLocal=false;
        for(size_t i=0;i<ents.size();i++) if(ents[i].local){ haveLocal=true; break; }
        if(!haveLocal && localIdx>=0 && localIdx<(int)ents.size()){
            ents[localIdx].local=true;
        }

        // Anchor the proximity fallback on the LOCAL character, not the camera,
        // so spectating or a long camera pull does not empty the list.
        Vec3 anchor = fr.cam.pos;
        for(size_t i=0;i<ents.size();i++) if(ents[i].local){ anchor=ents[i].root; break; }

        int mates=0;
        if(g_trace) g_traceCharsRead=(int)chars.size();
        for(size_t i=0;i<ents.size();i++){
            Entity& e=ents[i];
            const char* drop=nullptr;
            if(e.local) drop="local";                   // never draw ourselves

            // TEAM. Rivals does not use Player.Team; it is the TeamID attribute,
            // a single byte. Compare by inequality - the values are not fixed.
            bool teamKnown = !myTeam.empty() && !e.team.empty();
            bool mate      = teamKnown && e.team==myTeam;
            e.enemy = !mate;
            if(mate){ mates++; if(!drop && g_opt.hideTeam) drop="teammate"; }

            // ARENA. This server hosts several concurrent matches in ONE
            // DataModel, each offset into its own region of world space, so the
            // list has to be cut down to your own fight. EnvironmentID says
            // exactly which one you are in; proximity is the fallback for when
            // it is unset (the lobby, or a player who has not been assigned).
            if(!drop){
                if(g_opt.envFilter && !myEnv.empty() && !e.env.empty()){
                    if(e.env!=myEnv) drop="other-arena(env)";
                }else{
                    Vec3 d = sub(e.root, anchor);
                    if(fabsf(d.y) > ARENA_Y_BAND)                 drop="other-arena(Y)";
                    else if(dot(d,d) > ARENA_RADIUS*ARENA_RADIUS) drop="other-arena(dist)";
                }
            }
            if(!drop && e.health<=0) drop="dead(renderer-skips)";
            if(g_trace)
                g_traceRows.push_back({e.name,e.health,e.maxHealth,e.team,e.env,e.local,
                                       drop?drop:"DRAWN",true});
            if(drop && strcmp(drop,"dead(renderer-skips)")!=0) continue;
            fr.ents.push_back(e);
        }
        g_memSeen  = (int)ents.size();
        g_memArena = (int)fr.ents.size();
        g_memMates = mates;

        // ---- wall check, once per enemy per frame ----
        // The ESP dimming and the aimbot want the same answer.
        // It used to be computed independently in each of them - up to three
        // sweeps of ~1,470 boxes per enemy. One sweep now, cached on the Entity.
        if(g_geomParts>0 && (g_opt.aimWall || g_opt.espVis)){
            for(size_t i=0;i<fr.ents.size();i++){
                Entity& e=fr.ents[i];
                if(e.health<=0) continue;
                e.covHead=occluded(fr.cam.pos, aimPointFor(e,0));
                e.covBody=occluded(fr.cam.pos, aimPointFor(e,1));
            }
        }

        // Geometry for the wall check, anchored on him so the cache only ever
        // holds his own arena. Costs nothing when the aimbot is off.
        // The wall cache costs nothing when the aimbot is off.
        bool wantGeom = g_opt.aim && g_opt.aimWall;
        if(wantGeom) geomStep(anchor);
        else if(!g_geom.empty()){ g_geom.clear(); g_geomParts=0; gLast=0; }

        out=std::move(fr);
        return true;
    }
};

// ----------------------------- framebuffer -----------------------------
struct FB {
    int w=0,h=0; uint32_t* px=nullptr;   // 0xAARRGGBB, top-down. POINTS AT THE DIB BITS.
    // ---- dirty rectangle -------------------------------------------------
    // MEASURED before this existed: clear() memset 1920x1009x4 = 7.7 MB every
    // frame, present() memcpy'd the same 7.7 MB into the DIB, and
    // UpdateLayeredWindow pushed all of it to the compositor - ~3.7 ms of a
    // ~8 ms frame, for a screen that is 99% transparent. All three costs scale
    // with AREA, so the fix is to touch only the part that has anything on it:
    // every primitive widens a bounding box, the next frame clears only the
    // PREVIOUS box, and the layered window is resized to the CURRENT one, which
    // makes everything outside it stop existing rather than needing to be erased.
    int dx0=0,dy0=0,dx1=-1,dy1=-1;
    inline void dirty(int x0,int y0,int x1,int y1){
        if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=w)x1=w-1; if(y1>=h)y1=h-1;
        if(x0>x1||y0>y1) return;
        if(dx1<dx0){ dx0=x0; dy0=y0; dx1=x1; dy1=y1; return; }
        if(x0<dx0)dx0=x0; if(y0<dy0)dy0=y0; if(x1>dx1)dx1=x1; if(y1>dy1)dy1=y1;
    }
    inline bool hasDirty() const { return dx1>=dx0 && dy1>=dy0; }
    void resetDirty(){ dx0=0; dy0=0; dx1=-1; dy1=-1; }
    // Clear ONLY a rectangle, row by row. Called with last frame's dirty box.
    void clearRect(int x0,int y0,int x1,int y1){
        if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=w)x1=w-1; if(y1>=h)y1=h-1;
        if(x0>x1||y0>y1) return;
        size_t n=(size_t)(x1-x0+1)*4;
        for(int y=y0;y<=y1;y++) memset(px+(size_t)y*w+x0,0,n);
    }

    static inline uint32_t mix(uint32_t db, uint32_t c, uint32_t a){
        uint32_t ia=255-a;
        uint32_t sr=(c>>16)&0xFF, sg=(c>>8)&0xFF, sb=c&0xFF;
        uint32_t dr=(db>>16)&0xFF, dg=(db>>8)&0xFF, dbb=db&0xFF, da=(db>>24)&0xFF;
        uint32_t nr=(sr*a+dr*ia)/255, ng=(sg*a+dg*ia)/255, nb=(sb*a+dbb*ia)/255;
        uint32_t na=a+da*ia/255;
        return (na<<24)|(nr<<16)|(ng<<8)|nb;
    }
    // The per-pixel entry point. Bounds-checks, blends, and marks one pixel
    // dirty; the span writers below skip the per-pixel dirty call and mark the
    // whole span once, which is what makes filled rectangles cheap.
    inline void blend(int x,int y,uint32_t c){
        if((unsigned)x>=(unsigned)w||(unsigned)y>=(unsigned)h) return;
        uint32_t a=(c>>24)&0xFF; if(!a) return;
        dirty(x,y,x,y);
        uint32_t* d=&px[(size_t)y*w+x];
        *d = (a==255) ? c : mix(*d,c,a);
    }
    // Clipped horizontal span, one dirty mark, opaque fast path.
    inline void span(int x0,int x1,int y,uint32_t c){
        if((unsigned)y>=(unsigned)h) return;
        if(x0>x1){ int t=x0; x0=x1; x1=t; }
        if(x1<0||x0>=w) return;
        if(x0<0)x0=0; if(x1>=w)x1=w-1;
        uint32_t a=(c>>24)&0xFF; if(!a) return;
        dirty(x0,y,x1,y);
        uint32_t* d=px+(size_t)y*w+x0;
        int n=x1-x0+1;
        if(a==255){ for(int i=0;i<n;i++) d[i]=c; return; }
        for(int i=0;i<n;i++) d[i]=mix(d[i],c,a);
    }
    void hline(int x0,int x1,int y,uint32_t c){ span(x0,x1,y,c); }
    void vline(int x,int y0,int y1,uint32_t c){
        if((unsigned)x>=(unsigned)w) return;
        if(y0>y1){ int t=y0; y0=y1; y1=t; }
        if(y1<0||y0>=h) return;
        if(y0<0)y0=0; if(y1>=h)y1=h-1;
        uint32_t a=(c>>24)&0xFF; if(!a) return;
        dirty(x,y0,x,y1);
        uint32_t* d=px+(size_t)y0*w+x;
        if(a==255){ for(int y=y0;y<=y1;y++,d+=w) *d=c; return; }
        for(int y=y0;y<=y1;y++,d+=w) *d=mix(*d,c,a);
    }
    void rect(int x0,int y0,int x1,int y1,uint32_t c){ hline(x0,x1,y0,c); hline(x0,x1,y1,c); vline(x0,y0,y1,c); vline(x1,y0,y1,c); }
    void rectThick(int x0,int y0,int x1,int y1,uint32_t c){ rect(x0,y0,x1,y1,c); rect(x0+1,y0+1,x1-1,y1-1,c); }
    void fill(int x0,int y0,int x1,int y1,uint32_t c){
        if(y0>y1){ int t=y0; y0=y1; y1=t; }
        for(int y=y0;y<=y1;y++) span(x0,x1,y,c);
    }
    void circle(int cx,int cy,int r,uint32_t c){
        if(r<2) return;
        // Midpoint circle: eight symmetric pixels per step instead of a
        // trigonometric walk with a lround per point. Same picture, no sin/cos.
        int x=r, y=0, err=1-r;
        while(x>=y){
            blend(cx+x,cy+y,c); blend(cx+y,cy+x,c);
            blend(cx-y,cy+x,c); blend(cx-x,cy+y,c);
            blend(cx-x,cy-y,c); blend(cx-y,cy-x,c);
            blend(cx+y,cy-x,c); blend(cx+x,cy-y,c);
            y++;
            if(err<0) err+=2*y+1;
            else { x--; err+=2*(y-x)+1; }
        }
    }
    void line(int x0,int y0,int x1,int y1,uint32_t c){ // Bresenham
        dirty(x0<x1?x0:x1, y0<y1?y0:y1, x0<x1?x1:x0, y0<y1?y1:y0);
        int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
        for(;;){
            if((unsigned)x0<(unsigned)w && (unsigned)y0<(unsigned)h){
                uint32_t a=(c>>24)&0xFF;
                uint32_t* d=&px[(size_t)y0*w+x0];
                *d = (a==255)? c : mix(*d,c,a);
            }
            if(x0==x1&&y0==y1)break;
            int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;}
        }
    }
};

// ----------------------------- text (GDI glyph atlas -> ARGB) -----------------------------
// Rendered once at startup: each printable char rasterized by GDI, its coverage
// (antialiased white-on-black) stored so drawText can tint+alpha-blend it cheaply.
struct Glyph { int w,h; std::vector<uint8_t> cov; };
static Glyph g_glyphs[95];   // 0x20..0x7E
static int   g_fontH=0;

static void initFont(){
    HDC screen=GetDC(NULL);
    HDC dc=CreateCompatibleDC(screen);
    const int CW=32, CH=32;
    BITMAPINFO bi={}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=CW; bi.bmiHeader.biHeight=-CH; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HBITMAP dib=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    SelectObject(dc,dib);
    HFONT font=CreateFontA(16,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Tahoma");
    SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    TEXTMETRICA tm; GetTextMetricsA(dc,&tm); g_fontH=tm.tmHeight;
    uint32_t* p=(uint32_t*)bits;
    for(int c=32;c<127;c++){
        for(int i=0;i<CW*CH;i++) p[i]=0;
        char s[2]={(char)c,0};
        TextOutA(dc,0,0,s,1);
        SIZE sz; GetTextExtentPoint32A(dc,s,1,&sz);
        Glyph& g=g_glyphs[c-32]; g.w=sz.cx; g.h=g_fontH; g.cov.resize((size_t)g.w*g.h);
        for(int y=0;y<g.h;y++) for(int x=0;x<g.w;x++){
            uint32_t v = (x<CW&&y<CH)? p[y*CW+x] : 0;
            g.cov[y*g.w+x]=(uint8_t)(v&0xFF);   // white-on-black -> coverage
        }
    }
    DeleteObject(font); DeleteObject(dib); DeleteDC(dc); ReleaseDC(NULL,screen);
}
static int textW(const char* s){ int w=0; for(;*s;s++){ int c=*s; if(c<32||c>126)c=32; w+=g_glyphs[c-32].w; } return w; }
static void blitGlyph(FB& fb,int x,int y,const Glyph& g,uint32_t rgb,int alphaScale){
    // Clip and mark dirty ONCE for the whole glyph box, then write pixels with
    // no per-pixel bounds test - a name plus an HP number is ~200 glyph blits a
    // frame and each one used to pay four compares per covered pixel.
    if(x>=fb.w||y>=fb.h||x+g.w<=0||y+g.h<=0) return;
    fb.dirty(x,y,x+g.w-1,y+g.h-1);
    int gx0 = x<0?-x:0, gy0 = y<0?-y:0;
    int gx1 = (x+g.w>fb.w)? fb.w-x : g.w;
    int gy1 = (y+g.h>fb.h)? fb.h-y : g.h;
    uint32_t r=(rgb>>16)&0xFF, gg=(rgb>>8)&0xFF, b=rgb&0xFF;
    for(int gy=gy0;gy<gy1;gy++){
        const uint8_t* cov=&g.cov[(size_t)gy*g.w];
        uint32_t* d=fb.px+(size_t)(y+gy)*fb.w+(x+gx0);
        for(int gx=gx0;gx<gx1;gx++,d++){
            uint32_t a=cov[gx]; if(!a) continue;
            a=a*alphaScale/255; if(a>255)a=255;
            *d = (a==255)? ((255u<<24)|(r<<16)|(gg<<8)|b)
                         : FB::mix(*d,(r<<16)|(gg<<8)|b,a);
        }
    }
}
static void drawText(FB& fb,int x,int y,const char* s,uint32_t color,bool shadow=true){
    if(shadow){ int cx=x+1;
        for(const char* q=s;*q;q++){ int c=*q; if(c<32||c>126)c=32; const Glyph& g=g_glyphs[c-32];
            blitGlyph(fb,cx,y+1,g,0x000000,200); cx+=g.w; } }
    int cx=x;
    for(const char* q=s;*q;q++){ int c=*q; if(c<32||c>126)c=32; const Glyph& g=g_glyphs[c-32];
        blitGlyph(fb,cx,y,g,color&0xFFFFFF,255); cx+=g.w; }
}
static void drawTextC(FB& fb,int cx,int y,const char* s,uint32_t color){ drawText(fb,cx-textW(s)/2,y,s,color); }

// ----------------------------- globals + window -----------------------------
static FB g_fb;
static HWND g_hwnd;
static HBITMAP g_dib; static void* g_bits; static HDC g_memdc;
static bool g_running=true;

static uint32_t ARGB(int a,int r,int g,int b){ return ((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b; }

// The framebuffer IS the DIB, so there is no copy: drawing writes the bits the
// compositor will read. UpdateLayeredWindow is then given only the sub-rectangle
// that has something in it, and RESIZES the window to it - which is what makes
// everything outside that box disappear without anyone having to erase it.
//
// (x,y) is the Roblox client's top-left on screen; framebuffer coordinates stay
// anchored there whatever size the window ends up, so nothing else - menu hit
// testing, world-to-screen, the FOV circle - has to know this is happening.
static int g_presentW=0, g_presentH=0;
static void present(int x,int y){
    if(!g_fb.hasDirty()){
        // Nothing on screen this frame. Collapse to a 1x1 window rather than
        // pushing a full-size empty surface every frame while he is alone.
        if(g_presentW!=1){
            POINT s={0,0}; SIZE sz={1,1}; POINT d={x,y};
            BLENDFUNCTION bf={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            UpdateLayeredWindow(g_hwnd,NULL,&d,&sz,g_memdc,&s,0,&bf,ULW_ALPHA);
            g_presentW=1; g_presentH=1;
        }
        return;
    }
    int x0=g_fb.dx0, y0=g_fb.dy0, x1=g_fb.dx1, y1=g_fb.dy1;
    POINT ptSrc={x0,y0}; SIZE sz={x1-x0+1,y1-y0+1}; POINT ptDst={x+x0,y+y0};
    BLENDFUNCTION bf={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hwnd,NULL,&ptDst,&sz,g_memdc,&ptSrc,0,&bf,ULW_ALPHA);
    g_presentW=sz.cx; g_presentH=sz.cy;
}

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_DESTROY){ PostQuitMessage(0); return 0; }
    return DefWindowProc(h,m,w,l);
}


// Clears LAST frame's dirty box and starts a new one. Anything drawn outside the
// resulting box is not presented, so this is the only erase the overlay does.
static int g_prevX0=0,g_prevY0=0,g_prevX1=-1,g_prevY1=-1;
static void beginFrame(){
    if(g_prevX1>=g_prevX0) g_fb.clearRect(g_prevX0,g_prevY0,g_prevX1,g_prevY1);
    g_fb.resetDirty();
}
static void endFrame(){
    g_prevX0=g_fb.dx0; g_prevY0=g_fb.dy0; g_prevX1=g_fb.dx1; g_prevY1=g_fb.dy1;
}

static void drawEsp(const Frame& f){
    const Camera& c=f.cam;
    if(!g_opt.esp) return;
    // The wall check is only trustworthy if it can be SEEN to be right, so its
    // answer is drawn: a covered enemy gets a dim box, a visible one stays
    // bright. That makes a wrong occlusion result obvious on screen instead of
    // silently costing him targets.
    bool wallOn = g_opt.espVis && g_opt.aimWall && g_geomParts>0;
    for(const auto& e:f.ents){
        if(e.health<=0) continue;   // skip dead
        float tx,ty,td, fx,fy,fd;
        if(!worldToScreen(c,e.head,tx,ty,td)) continue;   // box top (head)
        if(!worldToScreen(c,e.root,fx,fy,fd)) continue;   // box bottom (feet)
        // project true width from the camera's right vector at the body center
        Vec3 center = muls(addv(e.head,e.root),0.5f);
        float lx,ly,ld,rrx,rry,rd2, halfW;
        if(worldToScreen(c,addv(center,muls(c.right,-e.width*0.5f)),lx,ly,ld) &&
           worldToScreen(c,addv(center,muls(c.right, e.width*0.5f)),rrx,rry,rd2))
            halfW=fabsf(rrx-lx)*0.5f;
        else halfW=fabsf(fy-ty)*0.22f;
        float cxf=(tx+fx)*0.5f;
        int y0=(int)(ty<fy?ty:fy), y1=(int)(ty<fy?fy:ty);
        if(y1-y0<14){ int m=(y0+y1)/2; y0=m-7; y1=m+7; }   // min size, far enemies
        if(halfW<3.5f) halfW=3.5f;
        int x0=(int)(cxf-halfW), x1=(int)(cxf+halfW), cx=(int)cxf;
        bool covered = wallOn && e.covHead && e.covBody;   // computed once, in poll()
        uint32_t col = e.enemy ? (covered ? ARGB(150,150,80,80) : ARGB(255,255,45,45))
                               : ARGB(255,60,180,255);

        float frac=e.maxHealth>0?e.health/e.maxHealth:0; if(frac<0)frac=0; if(frac>1)frac=1;
        uint32_t hc = frac>0.5f?ARGB(255,70,225,70):(frac>0.25f?ARGB(255,240,200,40):ARGB(255,240,60,40));

        if(g_opt.snapline){
            int syp = g_opt.snapCenter ? c.h/2 : c.h-1;
            g_fb.line(c.w/2, syp, cx, y1, ARGB(110,255,255,255));
        }
        if(g_opt.box){
            g_fb.rect(x0-1,y0-1,x1+1,y1+1,ARGB(210,0,0,0));   // dark outline
            g_fb.rectThick(x0,y0,x1,y1,col);
            g_fb.rect(x0+2,y0+2,x1-2,y1-2,ARGB(150,0,0,0));
        }
        if(g_opt.health){
            int hbx=x0-5, bh=y1-y0;
            g_fb.fill(hbx-1,y0-1,hbx+1,y1+1,ARGB(210,0,0,0));
            int hy2=y1-(int)(bh*frac);
            g_fb.fill(hbx,hy2,hbx,y1,hc);
        }
        if(g_opt.name) drawTextC(g_fb, cx, y0-g_fontH-3, e.name.c_str(), ARGB(255,240,240,240));
        if(g_opt.health){ char hp[16]; sprintf(hp,"%d",(int)(e.health+0.5f)); drawTextC(g_fb,cx,y1+3,hp,hc); }
    }
}

// ============================== AIMBOT ==============================
// This process never writes a byte of Rivals' memory - that is the entire point
// of the external (11.6) - so the only way to aim is to move the REAL mouse with
// SendInput. That keeps the anticheat surface at exactly zero and moves the whole
// risk to human observation, which is why every parameter he asked for
// (smoothing, FOV size, hold-to-aim) is a *behavioural* one.
//
// The loop is CLOSED. Each frame it re-reads the camera it is already reading
// for the ESP, takes the remaining ANGULAR error to the target, and nudges the
// mouse by a fraction of it. There is deliberately no world-angle-to-mouse-count
// constant anywhere: instead the loop MEASURES how many degrees of camera
// rotation one mouse count actually buys, so his in-game sensitivity is
// irrelevant and he can change it mid-match - it just re-converges in about 1 s.
struct AimState {
    bool  toggleOn=false, prevKey=false;
    bool  engaged=false, hasTarget=false;
    uint64_t targetId=0;
    std::string targetName;
    float tsx=0, tsy=0;              // target's screen position, for the marker
    // MEASURED, never configured: signed degrees of camera rotation per mouse
    // count, for the CURRENT zoom level. The sign carries the inversion, so a
    // flipped axis self-corrects. See gainFor() for why this is per-zoom.
    double gainX=0.15, gainY=-0.15;
    int    gbIdx=-1;                 // which bucket those came from, -1 = mid-transition
    float  lastFov=0; DWORD fovSteady=0;
    double lastSec=0;                // for frame-rate-independent smoothing
    float  lockDist=-1;              // screen distance of the held target, for the mode logic
    int    cmdX=0, cmdY=0;           // what we sent last frame
    double accX=0, accY=0;           // sub-count remainder, so slow drifts survive
    Vec3   pRight{}, pUp{}, pLook{}; bool havePrev=false;
    // Calibration accumulates over a ~400 ms window instead of frame to frame:
    // the overlay and the game run at different rates, so a single frame's
    // rotation may not yet contain the command that caused it. Summing both
    // sides over a window makes that pipeline delay cancel out.
    double wCmdX=0, wCmdY=0, wYaw=0, wPitch=0; DWORD wStart=0;
    int    calN=0;
    float  errDeg=0;
    int    candN=0;        // candidates considered this frame
    float  nearestPx=-1;   // closest one to the crosshair, FOV gate ignored
    int    dropFar=0, dropWall=0, switches=0;   // rejected: too far / behind cover; handovers
};
static AimState g_aim;

// The aimbot's flight recorder, same idea as overlay_auto_log.txt for the ESP:
// the live overlay writes its own engagement log so nothing has to be timed by
// hand. Measured why this exists: an `aimtest` window only engages while Rivals
// is the foreground window, and if he is reading the terminal while it runs it
// never engages at all - two runs in a row produced zero engaged frames. With
// this he just plays, and the log is there afterwards.
// ONE live log for the whole session - aim frames and any box that stopped
// being drawn - instead of the six separate files this used to spray next to
// the exe. He plays; `overlay_live.txt` is there afterwards.
static FILE* g_liveLog=nullptr;
static int   g_liveLines=0;
static const int LIVE_MAX=20000;
static DWORD g_liveT0=0;
static CRITICAL_SECTION g_liveCs;
static bool  g_liveCsInit=false;
static void liveLog(const char* fmt, ...){
    if(g_liveLines>=LIVE_MAX) return;
    if(!g_liveCsInit){ InitializeCriticalSection(&g_liveCs); g_liveCsInit=true; }
    EnterCriticalSection(&g_liveCs);
    if(!g_liveLog){
        g_liveLog=fopen("overlay_live.txt","w");
        if(!g_liveLog){ g_liveLines=LIVE_MAX; LeaveCriticalSection(&g_liveCs); return; }
        g_liveT0=GetTickCount();
        fputs("rivals-external live log.  AIM lines: err must FALL toward zero, gain must sit\n"
              "still per zoom bucket.  SILENT lines: err is the residual angle AT THE MOMENT\n"
              "the trigger was pulled - that is the number that decides whether it hit.\n\n",g_liveLog);
    }
    fprintf(g_liveLog,"%7.2fs  ",(GetTickCount()-g_liveT0)/1000.0);
    va_list ap; va_start(ap,fmt); vfprintf(g_liveLog,fmt,ap); va_end(ap);
    fputc('\n',g_liveLog);
    if(++g_liveLines%30==0) fflush(g_liveLog);
    LeaveCriticalSection(&g_liveCs);
}
static void aimLogWrite(){
    liveLog("AIM    %-18s err %6.3f deg  cmd %5d,%-5d  gain %+.4f/%+.4f  cal %3d  fov %5.1f  zoom %d",
            g_aim.targetName.c_str(), g_aim.errDeg, g_aim.cmdX, g_aim.cmdY,
            g_aim.gainX, g_aim.gainY, g_aim.calN, g_aim.lastFov, g_aim.gbIdx);
}

// Dry run: pick targets, compute the error, log what WOULD be sent - but never
// call SendInput. Lets target selection, the FOV gate and the projection be
// proven without touching his mouse. Only the gain loop needs real movement.
static bool g_aimDry=false;

static inline double clampd(double v,double lo,double hi){ return v<lo?lo:(v>hi?hi:v); }

// Degrees of camera rotation per mouse count is NOT one number - it is one per
// zoom level, because Rivals scales mouse sensitivity when he aims down sights.
//
// MEASURED over a full match with a single global estimate: it wandered from
// 0.063 to 0.714 deg/count, an 11x swing, because every ADS and un-ADS pulled it
// toward the other sensitivity and it never settled on either. That is exactly
// why smoothing felt right at hipfire and broken while zoomed - the commands
// were sized with the wrong sensitivity roughly half the time.
//
// So the gain is bucketed by the camera's own FOV, which is already read every
// frame for the projection. An unseen zoom level is seeded from the nearest
// known one scaled by the tangent ratio (halving the FOV usually halves the
// sensitivity), which is a prior, not an assumption - the loop measures it and
// corrects from there.
static void gainFor(float fov, bool mayCreate, int& idx, double& gx, double& gy){
    int best=-1; float bd=1e9f;
    for(int i=0;i<g_gbN;i++){ float d=fabsf(g_gb[i].fov-fov); if(d<bd){ bd=d; best=i; } }
    if(best>=0 && bd<GB_TOL){ idx=best; gx=g_gb[best].gx; gy=g_gb[best].gy; return; }

    double sx=0.15, sy=-0.15;
    if(best>=0){
        double k = tan(fov*0.5*3.14159265/180.0) / tan(g_gb[best].fov*0.5*3.14159265/180.0);
        if(k>0.05 && k<20.0){ sx=g_gb[best].gx*k; sy=g_gb[best].gy*k; }
        else                { sx=g_gb[best].gx;   sy=g_gb[best].gy;   }
    }
    // Only COMMIT a bucket when the FOV has settled. An ADS transition tweens
    // through a dozen intermediate values; storing each one would thrash the
    // table and teach it nonsense measured mid-tween.
    if(mayCreate){
        int slot;
        if(g_gbN<GB_MAX) slot=g_gbN++;
        else { slot=0; for(int i=1;i<g_gbN;i++) if(g_gb[i].n<g_gb[slot].n) slot=i; }
        g_gb[slot].fov=fov; g_gb[slot].gx=sx; g_gb[slot].gy=sy; g_gb[slot].n=0;
        idx=slot;
    } else idx=-1;
    gx=sx; gy=sy;
}

static void mouseMoveRel(int dx,int dy){
    if(!dx && !dy) return;
    INPUT in{}; in.type=INPUT_MOUSE;
    in.mi.dx=dx; in.mi.dy=dy; in.mi.dwFlags=MOUSEEVENTF_MOVE;
    SendInput(1,&in,sizeof(INPUT));
}

// ---------------------------- WALL CHECK ----------------------------
// Segment vs oriented box, with a bounding-sphere reject first. The sphere test
// throws away the overwhelming majority of parts in about ten flops, which is
// what keeps a few thousand boxes times a handful of targets free.
static bool segHitsOBB(const rv::OBB& b, Vec3 from, Vec3 dn, float t0, float t1){
    Vec3 m = sub(from, b.c);
    float ox=dot(m,b.ax), oy=dot(m,b.ay), oz=dot(m,b.az);
    float dx=dot(dn,b.ax), dy=dot(dn,b.ay), dz=dot(dn,b.az);
    const float* o[3]={&ox,&oy,&oz};
    const float* d[3]={&dx,&dy,&dz};
    float hh[3]={b.hx,b.hy,b.hz};
    float tmin=t0, tmax=t1;
    for(int i=0;i<3;i++){
        float di=*d[i], oi=*o[i], h=hh[i];
        if(fabsf(di)<1e-6f){ if(oi<-h || oi>h) return false; continue; }
        float inv=1.0f/di;
        float a=(-h-oi)*inv, bb=( h-oi)*inv;
        if(a>bb){ float t=a; a=bb; bb=t; }
        if(a>tmin) tmin=a;
        if(bb<tmax) tmax=bb;
        if(tmin>tmax) return false;
    }
    return true;
}

// Is the line from the camera to that point blocked by the world?
//   - the first 2.5 studs are ignored: his own weapon and the wall he is hugging
//     are not what stops the shot, and a false positive there would disable the
//     aimbot permanently while peeking.
//   - the last 1.5 studs are ignored: the target's own cover-adjacent geometry
//     and any box overlapping their body would otherwise always report blocked.
static int g_wallTests=0, g_wallBlocked=0;
static bool insideOBB(const rv::OBB& b, Vec3 p){
    Vec3 m=sub(p,b.c);
    return fabsf(dot(m,b.ax))<=b.hx && fabsf(dot(m,b.ay))<=b.hy && fabsf(dot(m,b.az))<=b.hz;
}

// Reports which box blocked, so the answer can be checked rather than believed.
static int  g_wallLastIdx=-1;
static float g_wallLastT=0;

static bool occluded(Vec3 from, Vec3 to){
    if(g_geom.empty()) return false;          // nothing scanned yet: never refuse a shot
    Vec3 d=sub(to,from);
    float len2=dot(d,d);
    if(len2<16.0f) return false;              // point blank
    float len=sqrtf(len2);
    Vec3 dn=muls(d,1.0f/len);
    float t0=2.5f, t1=len-1.5f;
    if(t1<=t0) return false;
    g_wallTests++;
    for(size_t i=0;i<g_geom.size();i++){
        const rv::OBB& b=g_geom[i];
        Vec3 m=sub(b.c,from);
        float t=dot(m,dn);
        if(t < t0-b.r || t > t1+b.r) continue;          // behind, or past the target
        float perp2 = dot(m,m) - t*t;
        if(perp2 > b.r*b.r) continue;                   // the ray misses its sphere
        // Standing inside a volume - an arena shell, a water block, a trigger -
        // does not stop a shot leaving it, and treating it as cover would block
        // literally everything.
        if(insideOBB(b,from) || insideOBB(b,to)) continue;
        if(segHitsOBB(b,from,dn,t0,t1)){
            g_wallBlocked++; g_wallLastIdx=(int)i; g_wallLastT=t;
            return true;
        }
    }
    return false;
}

// Where on a body to point. Falls back to the rig when a character has no
// hitbox parts, so a missing part can never cost a target.
static Vec3 aimPointFor(const Entity& e, int part){
    if(part==0)
        return e.hasAimHead ? e.aimHead : Vec3{ e.head.x, e.head.y-0.55f, e.head.z };
    return e.hasAimBody ? e.aimBody : muls(addv(e.head,e.root),0.5f);
}
static Vec3 aimPointOf(const Entity& e){ return aimPointFor(e,g_opt.aimPart); }

static void aimTick(const Frame& f, bool allowed){
    const Camera& c=f.cam;
    DWORD now=GetTickCount();

    // ---- 1. learn the gain from what the last commands actually did ----
    if(g_aim.havePrev){
        // Rotation since the last poll, expressed in the PREVIOUS frame's basis:
        // +yaw = the camera turned right, +pitch = it turned up.
        double dyaw = atan2((double)dot(c.look,g_aim.pRight),
                            (double)dot(c.look,g_aim.pLook)) * 57.29577951;
        double dpit = asin(clampd((double)dot(c.look,g_aim.pUp),-1.0,1.0)) * 57.29577951;
        // Accumulate ONLY on frames where we actually commanded that axis. The
        // camera also moves because HE is moving it, and the calibrator cannot
        // tell the two apart; measured in the first live run, a window that
        // straddled his own swing produced a ratio 20x off and dragged gainX
        // from 0.150 to 0.100 in one step for no good reason. Ignoring frames
        // where we sent nothing removes most of that contamination for free.
        if(g_aim.cmdX){ g_aim.wYaw   += dyaw; g_aim.wCmdX += g_aim.cmdX; }
        if(g_aim.cmdY){ g_aim.wPitch += dpit; g_aim.wCmdY += g_aim.cmdY; }
    }
    // Which zoom level are we on? An ADS transition tweens the FOV, so learning
    // is suspended until it has been steady for 150 ms - a measurement taken
    // mid-tween belongs to no sensitivity at all.
    if(fabsf(c.fovDeg-g_aim.lastFov) > 0.3f){
        g_aim.lastFov  = c.fovDeg;
        g_aim.fovSteady= now;
        g_aim.wCmdX=g_aim.wCmdY=g_aim.wYaw=g_aim.wPitch=0;   // discard the straddling window
        g_aim.wStart   = now;
    }
    bool fovStable = (now - g_aim.fovSteady) > 150;
    gainFor(c.fovDeg, fovStable, g_aim.gbIdx, g_aim.gainX, g_aim.gainY);

    if(!g_aim.wStart) g_aim.wStart=now;
    if(now-g_aim.wStart > 400 && fovStable && g_aim.gbIdx>=0){
        // Thresholds are low so BOTH axes get windows. The first live run only
        // ever calibrated yaw: pitch corrections are small, so |wCmdY| never
        // reached the old 25-count bar and gainY sat on its seed all match,
        // which means vertical was permanently 1.5x too hot.
        //
        // The ratio is then sanity-checked against what we already believe. A
        // window contaminated by his own mouse produces a wild ratio, and the
        // old code accepted anything inside 0.0008..5.0 - far too wide. Staying
        // within 6x of the current estimate still lets a badly wrong seed be
        // corrected in two or three windows, while a contaminated one is simply
        // dropped. Sign must match too: our own commands cannot have turned the
        // camera the other way.
        GainBucket& b=g_gb[g_aim.gbIdx];
        if(fabs(g_aim.wCmdX)>=8 && fabs(g_aim.wYaw)>0.3){
            double r=g_aim.wYaw/g_aim.wCmdX;
            if(r*b.gx>0 && fabs(r)>fabs(b.gx)/6.0 && fabs(r)<fabs(b.gx)*6.0){
                b.gx += (r-b.gx)*0.30; b.n++; g_aim.calN++;
            }
        }
        if(fabs(g_aim.wCmdY)>=8 && fabs(g_aim.wPitch)>0.3){
            double r=g_aim.wPitch/g_aim.wCmdY;
            if(r*b.gy>0 && fabs(r)>fabs(b.gy)/6.0 && fabs(r)<fabs(b.gy)*6.0){
                b.gy += (r-b.gy)*0.30; b.n++; g_aim.calN++;
            }
        }
        g_aim.gainX=b.gx; g_aim.gainY=b.gy;
        g_aim.wCmdX=g_aim.wCmdY=g_aim.wYaw=g_aim.wPitch=0; g_aim.wStart=now;
    }
    g_aim.pRight=c.right; g_aim.pUp=c.up; g_aim.pLook=c.look; g_aim.havePrev=true;
    g_aim.cmdX=g_aim.cmdY=0;
    g_aim.engaged=false; g_aim.hasTarget=false;
    g_aim.candN=0; g_aim.nearestPx=-1; g_aim.dropFar=0; g_aim.dropWall=0;

    if(!allowed || c.w<=0 || c.h<=0){
        g_aim.accX=g_aim.accY=0; g_aim.targetId=0; g_aim.targetName.clear();
        return;
    }

    // ---- 2. pick a target ----
    // The list handed in is ALREADY enemies-only, in-arena and not us (10.3),
    // but the checks are repeated here so a future change to the ESP filter can
    // never quietly turn this into a teammate-aimbot.
    float ccx=c.w*0.5f, ccy=c.h*0.5f;
    float fovR  = g_opt.aimFov ? (float)g_opt.aimFovPx : 1e9f;
    // In LOCK mode the held target only has to stay inside the FOV circle; in
    // CLOSEST mode it gets a 1.35x grace ring so a target skimming the edge does
    // not drop and re-acquire every other frame.
    float keepR = g_opt.aimFov ? (g_opt.aimTarget==1 ? fovR : fovR*1.35f) : 1e9f;
    float maxD2 = g_opt.aimMaxDist>0 ? (float)g_opt.aimMaxDist*(float)g_opt.aimMaxDist : 1e30f;

    const Entity* best=nullptr; float bestD=1e18f; Vec3 bestPt{}; float bsx=0,bsy=0;
    const Entity* cur =nullptr; float curD =1e18f; Vec3 curPt{};  float csx=0,csy=0;

    for(size_t i=0;i<f.ents.size();i++){
        const Entity& e=f.ents[i];
        if(e.local || !e.enemy || e.health<=0) continue;
        Vec3 pt=aimPointFor(e,g_opt.aimPart);
        Vec3 dv=sub(pt,c.pos);
        if(dot(dv,dv)>maxD2){ g_aim.dropFar++; continue; }   // too far to be his fight
        // WALL CHECK. If the chosen hitbox is behind cover, try the other one
        // before giving up - a head poking over a crate is a shot, and so is a
        // body under a railing. Only when BOTH are blocked is the target dropped.
        // The answers themselves come from poll(), which computes them once.
        if(g_opt.aimWall && g_geomParts>0){
            bool covThis = g_opt.aimPart ? e.covBody : e.covHead;
            bool covAlt  = g_opt.aimPart ? e.covHead : e.covBody;
            if(covThis){
                if(covAlt){ g_aim.dropWall++; continue; }
                pt=aimPointFor(e,g_opt.aimPart?0:1);
            }
        }
        float sx,sy,dep;
        if(!worldToScreen(c,pt,sx,sy,dep)) continue;         // behind the camera
        float dx=sx-ccx, dy=sy-ccy;
        float d=sqrtf(dx*dx+dy*dy);
        g_aim.candN++;
        if(g_aim.nearestPx<0 || d<g_aim.nearestPx) g_aim.nearestPx=d;
        bool isCur = g_aim.targetId ? (e.id==g_aim.targetId)
                                    : (!g_aim.targetName.empty() && e.name==g_aim.targetName);
        if(isCur && d<=keepR){ cur=&e; curD=d; curPt=pt; csx=sx; csy=sy; }
        if(d<=fovR && d<bestD){ best=&e; bestD=d; bestPt=pt; bsx=sx; bsy=sy; }
    }

    // TWO SELECTION MODES, because they want opposite things.
    //  CLOSEST - track whatever is nearest the crosshair, but only hand over when
    //            the newcomer is CLEARLY closer (60%). Equal-distance rivals used
    //            to swap every frame, which is what felt glitchy at low smoothing:
    //            with a small divisor each swap is a full-speed flick.
    //  LOCK    - acquire once, then hold that player anywhere in the FOV until he
    //            dies, leaves the circle, or the bind is released. Never hands
    //            over to a closer one.
    const Entity* tgt=nullptr; Vec3 tpt{}; float tsx=0, tsy=0;
    if(cur && (g_opt.aimTarget==1 || !best || bestD >= curD*0.6f)){
        tgt=cur; tpt=curPt; tsx=csx; tsy=csy;
    }else if(best){
        tgt=best; tpt=bestPt; tsx=bsx; tsy=bsy;
        if(cur && tgt!=cur) g_aim.switches++;
    }
    if(!tgt){
        g_aim.targetId=0; g_aim.targetName.clear(); g_aim.accX=g_aim.accY=0;
        return;
    }
    g_aim.tsx=tsx; g_aim.tsy=tsy;
    if(tgt->id!=g_aim.targetId){ g_aim.accX=g_aim.accY=0; }   // no carry-over onto a new target
    g_aim.targetId=tgt->id; g_aim.targetName=tgt->name; g_aim.hasTarget=true;

    // ---- 3. angular error, straight out of the camera basis ----
    Vec3 d3=sub(tpt,c.pos);
    double cx=dot(d3,c.right), cy=dot(d3,c.up), cz=dot(d3,c.look);
    if(cz<0.05){ g_aim.accX=g_aim.accY=0; return; }
    double yawErr = atan2(cx,cz)*57.29577951;                    // + = target is right
    double pitErr = atan2(cy,sqrt(cx*cx+cz*cz))*57.29577951;     // + = target is above
    g_aim.errDeg  = (float)sqrt(yawErr*yawErr+pitErr*pitErr);

    // ---- 4. move a fraction of it ----
    // Smoothing is a TIME constant, not a per-frame divisor. The old
    // `error / smoothing` corrected 1/8 of the error per frame, so the same
    // slider setting felt different at 50 fps than at 40 - and the overlay's own
    // rate moves with his game. Now: alpha = 1 - exp(-dt/tau), which converges at
    // the same real-world rate whatever the frame rate. tau is picked so a given
    // slider value feels the same as it did at 45 fps before this change.
    double s  = g_opt.aimSmooth<1 ? 1.0 : (double)g_opt.aimSmooth;
    double t  = nowSec();
    double dt = (g_aim.lastSec>0) ? (t-g_aim.lastSec) : (1.0/45.0);
    g_aim.lastSec=t;
    if(dt<0.001) dt=0.001; if(dt>0.25) dt=0.25;      // a hitch must not become a flick
    double tau   = s/45.0;
    double alpha = 1.0 - exp(-dt/tau);
    if(alpha>0.90) alpha=0.90;

    double gx = g_aim.gainX, gy = g_aim.gainY;
    if(fabs(gx)<0.0008) gx = gx<0?-0.0008:0.0008;
    if(fabs(gy)<0.0008) gy = gy<0?-0.0008:0.0008;
    g_aim.accX += (yawErr*alpha)/gx;
    g_aim.accY += (pitErr*alpha)/gy;
    int mx=(int)(g_aim.accX>=0?floor(g_aim.accX):ceil(g_aim.accX));
    int my=(int)(g_aim.accY>=0?floor(g_aim.accY):ceil(g_aim.accY));
    g_aim.accX-=mx; g_aim.accY-=my;
    const int MAXC=260;                    // never fling the view across the map
    if(mx> MAXC) mx= MAXC;
    if(mx<-MAXC) mx=-MAXC;
    if(my> MAXC) my= MAXC;
    if(my<-MAXC) my=-MAXC;
    if(fabs(yawErr)<0.03) mx=0;            // deadzone: sub-pixel corrections are jitter
    if(fabs(pitErr)<0.03) my=0;
    g_aim.cmdX=mx; g_aim.cmdY=my;
    g_aim.engaged=true;
    if(g_aimDry){ g_aim.cmdX=g_aim.cmdY=0; return; }   // do not feed the calibrator either
    mouseMoveRel(mx,my);
}

// FOV circle + a marker on whatever is being tracked. Drawn independently of the
// ESP master so he can run aim with the boxes off.
static void aimDraw(FB& fb, const Frame& f){
    if(!g_opt.aim) return;
    const Camera& c=f.cam;
    int ccx=c.w/2, ccy=c.h/2;
    if(g_opt.aimFov && g_opt.aimFovDraw)
        fb.circle(ccx,ccy,g_opt.aimFovPx,
                  g_aim.engaged?ARGB(200,255,90,90):ARGB(105,175,185,200));
    if(g_aim.engaged && g_aim.hasTarget){
        int x=(int)g_aim.tsx, y=(int)g_aim.tsy;
        fb.hline(x-8,x+8,y,ARGB(230,255,120,120));
        fb.vline(x,y-8,y+8,ARGB(230,255,120,120));
        fb.rect(x-3,y-3,x+3,y+3,ARGB(190,255,205,120));
    }
}

// ---------------------- immediate-mode widgets ----------------------
// The old menu was a fixed column of checkboxes. The aimbot needs controls that
// cannot express: a slider that drags, radio groups, and a field that captures a
// key. All of it is immediate-mode - one call draws AND handles the click.
static int g_entCount=0; static int g_fpsShown=0; static bool g_live=false;
static DWORD g_f7Flash=0, g_startTk=0;
static int  g_uiDrag=-1;     // id of the slider currently being dragged, -1 none
static int  g_uiCapture=0;   // 1 while a bind widget is waiting for a key press
static int* g_uiCaptureTgt=nullptr;  // WHICH bind - there are two of them now

struct UICtx { FB* fb; int mx,my; bool down, click; };
static UICtx g_ui;
static bool uiHit(int x0,int y0,int x1,int y1){
    return g_ui.mx>=x0 && g_ui.mx<=x1 && g_ui.my>=y0 && g_ui.my<=y1;
}
static const int UI_CHECK_H = 24;

static void uiCheck(int x,int y,int w,const char* label,bool* v){
    bool over=uiHit(x-6,y-3,x+w,y+19);
    if(over) g_ui.fb->fill(x-6,y-3,x+w,y+19, ARGB(60,120,160,220));
    g_ui.fb->rect(x,y,x+16,y+16, ARGB(255,140,150,170));
    if(*v) g_ui.fb->fill(x+3,y+3,x+13,y+13, ARGB(255,90,205,120));
    drawText(*g_ui.fb, x+26, y+1, label, over?ARGB(255,255,255,255):ARGB(255,205,208,214));
    if(over && g_ui.click){ *v=!*v; g_cfgDirty=true; }
}

// Returns the height it used, so the callers can stack without magic numbers.
static int uiSlider(int id,int x,int y,int w,const char* label,int* v,int lo,int hi,const char* fmt){
    char buf[64]; snprintf(buf,sizeof(buf),fmt,*v);
    drawText(*g_ui.fb, x, y, label, ARGB(255,190,195,205));
    drawText(*g_ui.fb, x+w-textW(buf), y, buf, ARGB(255,120,200,255));
    int ty=y+g_fontH+6, tx0=x, tx1=x+w;
    bool over=uiHit(tx0-5,ty-7,tx1+5,ty+10);
    if(g_ui.click && over) g_uiDrag=id;
    if(!g_ui.down && g_uiDrag==id) g_uiDrag=-1;
    if(g_uiDrag==id){
        float t=(float)(g_ui.mx-tx0)/(float)(tx1-tx0>0?tx1-tx0:1);
        if(t<0)t=0; if(t>1)t=1;
        int nv=lo+(int)(t*(hi-lo)+0.5f);
        if(nv!=*v){ *v=nv; g_cfgDirty=true; }
    }
    float frac=(float)(*v-lo)/(float)(hi>lo?hi-lo:1);
    int kx=tx0+(int)(frac*(tx1-tx0));
    g_ui.fb->fill(tx0,ty,tx1,ty+3, ARGB(255,46,52,64));
    if(kx>tx0) g_ui.fb->fill(tx0,ty,kx,ty+3, ARGB(255,70,150,230));
    g_ui.fb->fill(kx-3,ty-4,kx+3,ty+7,
                  (g_uiDrag==id||over)?ARGB(255,225,238,255):ARGB(255,170,190,215));
    return g_fontH+6+12+8;
}

static int uiRadio(int x,int y,int w,const char* label,const char* const* opts,int n,int* v){
    drawText(*g_ui.fb, x, y, label, ARGB(255,190,195,205));
    int yy=y+g_fontH+5;
    int bw=(w-(n-1)*6)/n;
    for(int i=0;i<n;i++){
        int bx=x+i*(bw+6);
        bool over=uiHit(bx,yy,bx+bw,yy+18), on=(*v==i);
        g_ui.fb->fill(bx,yy,bx+bw,yy+18,
                      on?ARGB(255,38,92,148):(over?ARGB(255,46,52,64):ARGB(255,28,32,40)));
        g_ui.fb->rect(bx,yy,bx+bw,yy+18, on?ARGB(255,110,180,255):ARGB(255,68,74,88));
        drawTextC(*g_ui.fb, bx+bw/2, yy+2, opts[i],
                  on?ARGB(255,235,245,255):ARGB(255,170,176,188));
        if(over && g_ui.click){ *v=i; g_cfgDirty=true; }
    }
    return g_fontH+5+18+8;
}

static const char* vkName(int vk){
    static char b[24];
    switch(vk){
        case VK_LBUTTON:  return "MOUSE1";
        case VK_RBUTTON:  return "MOUSE2";
        case VK_MBUTTON:  return "MOUSE3";
        case VK_XBUTTON1: return "MOUSE4";
        case VK_XBUTTON2: return "MOUSE5";
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:    return "SHIFT";
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL";
        case VK_MENU: case VK_LMENU: case VK_RMENU:       return "ALT";
        case VK_SPACE:   return "SPACE";
        case VK_TAB:     return "TAB";
        case VK_CAPITAL: return "CAPS";
        case VK_OEM_3:   return "GRAVE";
    }
    if(vk>=VK_F1 && vk<=VK_F24){ snprintf(b,sizeof(b),"F%d",vk-VK_F1+1); return b; }
    if((vk>='A'&&vk<='Z')||(vk>='0'&&vk<='9')){ b[0]=(char)vk; b[1]=0; return b; }
    snprintf(b,sizeof(b),"VK %02X",vk); return b;
}

static int uiKeyBind(int x,int y,int w,const char* label,int* vk){
    drawText(*g_ui.fb, x, y, label, ARGB(255,190,195,205));
    int yy=y+g_fontH+5;
    bool over=uiHit(x,yy,x+w,yy+18), cap=(g_uiCapture!=0 && g_uiCaptureTgt==vk);
    g_ui.fb->fill(x,yy,x+w,yy+18,
                  cap?ARGB(255,86,58,20):(over?ARGB(255,46,52,64):ARGB(255,28,32,40)));
    g_ui.fb->rect(x,yy,x+w,yy+18, cap?ARGB(255,255,190,90):ARGB(255,68,74,88));
    drawTextC(*g_ui.fb, x+w/2, yy+2, cap?"press a key (ESC cancels)":vkName(*vk),
              cap?ARGB(255,255,215,150):ARGB(255,215,220,230));
    if(over && g_ui.click){ g_uiCapture=1; g_uiCaptureTgt=vk; }
    return g_fontH+5+18+8;
}

// In-overlay menu. Drawn when open; mutates g_opt. mx,my are cursor coords in
// window space; click is a press edge, down is the held state (sliders need it).

// The card the user actually sees before the boxes appear. Without it, launching
// the overlay before Roblox looks identical to launching a broken exe.
static void drawStatus(FB& fb, bool live, DWORD attachedAt){
    if(live && (!attachedAt || GetTickCount()-attachedAt > 4000)) return;
    const char* line1;
    uint32_t accent;
    if(live)            { line1="ATTACHED";                        accent=ARGB(255,120,230,150); }
    else if(rv::g_h)    { line1="reading memory...";               accent=ARGB(255,255,205,120); }
    else if(robloxPid()){ line1="found Roblox - attaching...";      accent=ARGB(255,255,205,120); }
    else                { line1="waiting for Roblox...";            accent=ARGB(255,170,185,205); }

    const int x=40, y=40, w=352, h=84;
    fb.fill(x,y,x+w,y+h, ARGB(225,16,18,24));
    fb.rect(x,y,x+w,y+h, accent);
    fb.fill(x+1,y+1,x+w-1,y+24, ARGB(255,32,60,96));
    drawText(fb, x+12, y+3,  "RIVALS EXTERNAL", ARGB(255,150,210,255));
    drawText(fb, x+12, y+32, line1, accent);
    drawText(fb, x+12, y+52, live ? "INSERT menu   F7 log   END quit"
                                  : "it will attach by itself   END quit",
             ARGB(200,140,145,160));
}

static void drawMenu(FB& fb, int mx, int my, bool click, bool down){
    g_ui.fb=&fb; g_ui.mx=mx; g_ui.my=my; g_ui.click=click; g_ui.down=down;
    if(!down) g_uiDrag=-1;

    const int px=28, py=28, pw=568, ph=644;
    const int colW=238, c0=px+18, c1=px+18+colW+34;

    fb.fill(px,py,px+pw,py+ph, ARGB(235,16,18,24));
    fb.rect(px,py,px+pw,py+ph, ARGB(255,70,120,180));
    fb.rect(px+1,py+1,px+pw-1,py+ph-1, ARGB(120,40,60,90));
    fb.fill(px+1,py+1,px+pw-1,py+28, ARGB(255,32,60,96));
    drawText(fb, px+12, py+7, "RIVALS EXTERNAL", ARGB(255,150,210,255));

    char st[160], tid[24]="-", eid[24]="-";
    if(!g_myTeam.empty()) sprintf(tid,"%02X",(unsigned char)g_myTeam[0]);
    if(!g_myEnv.empty())  sprintf(eid,"%02X",(unsigned char)g_myEnv[0]);
    sprintf(st,"%s  %d fps  %d enemies  team %s/env %s",
            g_srcTag, g_fpsShown, g_entCount, tid, eid);
    drawText(fb, px+12, py+32, st, ARGB(255,150,160,175));
    fb.vline(px+18+colW+17, py+52, py+ph-34, ARGB(70,90,110,140));

    // ---------------- left column: ESP ----------------
    int y=py+54;
    drawText(fb, c0, y, "ESP", ARGB(255,120,200,255)); y+=g_fontH+8;
    uiCheck(c0,y,colW,"ESP master",       &g_opt.esp);        y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Boxes",            &g_opt.box);        y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Names",            &g_opt.name);       y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Health",           &g_opt.health);     y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Snaplines",        &g_opt.snapline);   y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Snap from center", &g_opt.snapCenter); y+=UI_CHECK_H;
    y+=8;
    drawText(fb, c0, y, "FILTERS", ARGB(255,120,200,255)); y+=g_fontH+8;
    uiCheck(c0,y,colW,"Hide teammates",   &g_opt.hideTeam);   y+=UI_CHECK_H;
    uiCheck(c0,y,colW,"Arena filter",     &g_opt.envFilter);  y+=UI_CHECK_H;

    // ---------------- right column: AIMBOT ----------------
    static const char* PART_OPTS[] = { "Head", "Torso" };
    static const char* MODE_OPTS[] = { "Always", "Hold", "Toggle" };
    static const char* PICK_OPTS[] = { "Closest", "Lock" };
    y=py+54;
    drawText(fb, c1, y, "AIMBOT", ARGB(255,255,150,110)); y+=g_fontH+8;
    uiCheck (c1,y,colW,"Enable aimbot", &g_opt.aim);  y+=UI_CHECK_H+6;
    y+=uiRadio  (c1,y,colW,"Target",     PART_OPTS,2,&g_opt.aimPart);
    y+=uiRadio  (c1,y,colW,"Activation", MODE_OPTS,3,&g_opt.aimMode);
    y+=uiKeyBind(c1,y,colW,"Bind",                    &g_opt.aimKey);
    y+=uiRadio  (c1,y,colW,"Selection", PICK_OPTS,2,&g_opt.aimTarget);
    y+=uiSlider (1,c1,y,colW,"Smoothing",&g_opt.aimSmooth,1,20,"%d");
    uiCheck (c1,y,colW,"FOV limit", &g_opt.aimFov);      y+=UI_CHECK_H;
    y+=uiSlider (2,c1,y,colW,"FOV radius",&g_opt.aimFovPx,20,600,"%d px");
    y+=uiSlider (3,c1,y,colW,"Max distance",&g_opt.aimMaxDist,0,1500,
                 g_opt.aimMaxDist?"%d studs":"off");
    uiCheck (c1,y,colW,"Wall check", &g_opt.aimWall);         y+=UI_CHECK_H;
    uiCheck (c1,y,colW,"Dim covered enemies", &g_opt.espVis);  y+=UI_CHECK_H;
    uiCheck (c1,y,colW,"Draw FOV circle", &g_opt.aimFovDraw); y+=UI_CHECK_H+4;

    // What the closed loop has actually learned. Worth showing: if the gain is
    // still at its 0.150 seed, the loop has not calibrated yet and aim will feel
    // slow for the first second of the first engagement.
    // The gain readout is per ZOOM LEVEL now. `zoom 3/4` = this is the 4th FOV
    // it has seen and the 3rd bucket is live; a `-` means the FOV is mid-tween
    // and learning is paused.
    char ai[160];
    snprintf(ai,sizeof(ai),"fov %.0f  zoom %s%d/%d  gain %.3f/%.3f  (%d)",
             g_aim.lastFov, g_aim.gbIdx<0?"-":"", g_aim.gbIdx<0?0:g_aim.gbIdx+1,
             g_gbN, g_aim.gainX, g_aim.gainY, g_aim.calN);
    drawText(fb, c1, y, ai, ARGB(200,140,150,168)); y+=g_fontH+3;
    snprintf(ai,sizeof(ai),"walls %d parts (%d nodes)  dropped: %d far, %d covered",
             g_geomParts, g_geomNodes, g_aim.dropFar, g_aim.dropWall);
    drawText(fb, c1, y, ai, ARGB(200,140,150,168)); y+=g_fontH+3;
    if(g_aim.hasTarget) snprintf(ai,sizeof(ai),"%s  %s  err %.2f deg",
                                 g_aim.engaged?"TRACKING":"target",
                                 g_aim.targetName.c_str(), g_aim.errDeg);
    else                snprintf(ai,sizeof(ai),"no target");
    drawText(fb, c1, y, ai, g_aim.engaged?ARGB(230,255,170,120):ARGB(190,140,150,168));

    drawText(fb, px+12, py+ph-24, "INSERT menu   F7 log   END quit   (settings saved to config.ini)",
             ARGB(200,140,145,160));
}

// One surface, not two. The DIB section's bits ARE the framebuffer, which
// removes a 7.7 MB memcpy per frame and halves the overlay's memory footprint.
static void makeSurface(int w,int h){
    if(g_memdc){ if(g_dib) DeleteObject(g_dib); DeleteDC(g_memdc); g_memdc=NULL; g_dib=NULL; }
    BITMAPINFO bi={}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    HDC screen=GetDC(NULL); g_memdc=CreateCompatibleDC(screen);
    g_dib=CreateDIBSection(g_memdc,&bi,DIB_RGB_COLORS,&g_bits,NULL,0);
    SelectObject(g_memdc,g_dib); ReleaseDC(NULL,screen);
    g_fb.w=w; g_fb.h=h; g_fb.px=(uint32_t*)g_bits;
    g_fb.resetDirty(); g_presentW=g_presentH=0;
    if(g_bits) memset(g_bits,0,(size_t)w*h*4);
}

// Find the Roblox window by PROCESS (its title is unreliable), pick the visible
// top-level window with the largest client area, return its client rect on screen.
static DWORD robloxPid(){
    DWORD pid=0; HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(s==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if(Process32FirstW(s,&pe)) do{
        if(_wcsicmp(pe.szExeFile,L"RobloxPlayerBeta.exe")==0){ pid=pe.th32ProcessID; break; }
    }while(Process32NextW(s,&pe));
    CloseHandle(s); return pid;
}
struct EnumCtx{ DWORD pid; HWND best; long area; };
static BOOL CALLBACK enumCb(HWND h,LPARAM lp){
    EnumCtx* c=(EnumCtx*)lp; DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(pid!=c->pid || !IsWindowVisible(h)) return TRUE;
    RECT rc; if(!GetClientRect(h,&rc)) return TRUE;
    long a=(long)(rc.right-rc.left)*(rc.bottom-rc.top);
    if(a>c->area){ c->area=a; c->best=h; }
    return TRUE;
}
static bool robloxClient(int& x,int& y,int& w,int& h){
    // Resolving this from scratch costs a full process-list snapshot plus an
    // EnumWindows sweep over every top-level window - about 12 ms - and it was
    // being paid TWICE per frame (once by the render loop, once inside the data
    // source). That, not the rasterizer and not the memory reads, was where the
    // overlay's entire CPU budget was going. The handle is stable for the life
    // of the client, so cache it and only re-resolve when it stops being a window.
    static HWND cached=NULL;
    if(cached && !IsWindow(cached)) cached=NULL;
    if(!cached){
        DWORD pid=robloxPid(); if(!pid) return false;
        EnumCtx c{pid,NULL,0}; EnumWindows(enumCb,(LPARAM)&c);
        if(!c.best) return false;
        cached=c.best;
    }
    g_rbxWnd=cached;
    RECT rc;
    if(!GetClientRect(cached,&rc)){ cached=NULL; return false; }
    POINT tl={0,0}; ClientToScreen(cached,&tl);
    x=tl.x; y=tl.y; w=rc.right-rc.left; h=rc.bottom-rc.top; return true;
}

// ============================ THE RE WORKSHOP ============================
// This used to be a second program, rvscan.exe, with its own copy of attach /
// ReadProcessMemory / the RTTI walker / the tree walker. All of that already
// exists in rv:: above, so the second copy was 950 lines of duplicate that could
// drift out of step with the one the overlay actually runs on. These are the
// same commands rebuilt on top of the live code:
//
//   overlay.exe scan info
//   overlay.exe scan needle <hex..>            multi-needle, one pass
//   overlay.exe scan ptr <addr..>              who points AT these
//   overlay.exe scan ptrin <base> <size>       who points ANYWHERE INSIDE
//   overlay.exe scan obj <addr> [maxback]      backscan for a vftable
//   overlay.exe scan cmp <addr..>              slot-by-slot SAME/DIFF
//   overlay.exe scan read <addr> <bytes>       hex + ASCII
//   overlay.exe scan rtti <addr>               class + full base-class list
//   overlay.exe scan classes [substr]          every RTTI vftable in the module
//   overlay.exe scan vfscan <vftable>          every heap object with that vftable
//   overlay.exe scan dm [depth]                self-locate the DataModel, walk it
//   overlay.exe scan tree <inst> [depth]
//   overlay.exe scan findname <root> <substr> [depth]
//   overlay.exe scan attrs <inst>              every attribute: name, type, value
//   overlay.exe scan path <start> <qword> [depth]   BFS the pointer graph
//
// Output goes to overlay_log.txt and, when launched from a console, to the
// console as well - this is a /SUBSYSTEM:WINDOWS binary, so it has to ask.
static FILE* g_out=nullptr;
static bool  g_outCon=false;
static void outOpen(const char* path){
    g_out=fopen(path,"w");
    if(AttachConsole(ATTACH_PARENT_PROCESS)){
        g_outCon=true;
        freopen("CONOUT$","w",stdout);
    }
}
static void P(const char* fmt, ...){
    va_list ap;
    if(g_out){ va_start(ap,fmt); vfprintf(g_out,fmt,ap); va_end(ap); }
    if(g_outCon){ va_start(ap,fmt); vfprintf(stdout,fmt,ap); va_end(ap); }
}
static void outClose(){
    if(g_out){ fclose(g_out); g_out=nullptr; }
    if(g_outCon){ fflush(stdout); FreeConsole(); g_outCon=false; }
}

static uint64_t Hex(const char* s){
    if(!s) return 0;
    while(*s==' ') s++;
    if(s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
    return _strtoui64(s,nullptr,16);
}
// Decimal-or-hex, for byte counts and depths where 10 should mean ten.
static uint64_t Num(const char* s){
    if(!s) return 0;
    if(s[0]=='0'&&(s[1]=='x'||s[1]=='X')) return _strtoui64(s+2,nullptr,16);
    return _strtoui64(s,nullptr,10);
}

// One chunked pass over every committed readable region outside the module.
// Everything that scans memory goes through this, so there is exactly one copy
// of the "reuse the buffer, never resize per region" lesson from HANDOFF 11.5.
template<class F>
static void sweep(F fn, bool includeModule=false){
    const size_t CHUNK=4u<<20;
    std::vector<uint8_t> buf(CHUNK);
    for(auto& r : rv::regions()){
        if(!includeModule && r.base>=rv::g_base && r.base<rv::g_base+rv::g_size) continue;
        for(size_t off=0; off<r.size; off+=CHUNK){
            size_t n=r.size-off; if(n>CHUNK) n=CHUNK;
            SIZE_T got=0;
            if(!ReadProcessMemory(rv::g_h,(LPCVOID)(r.base+off),buf.data(),n,&got) || !got) continue;
            fn((uint64_t)(r.base+off), buf.data(), (size_t)got);
        }
    }
}

static void wsHexDump(uint64_t addr, size_t n){
    std::vector<uint8_t> b(n);
    size_t got=0;
    for(size_t o=0;o<n;o+=0x1000){
        size_t c=n-o; if(c>0x1000) c=0x1000;
        if(!rv::rd(addr+o,b.data()+o,c)) break;
        got=o+c;
    }
    for(size_t o=0;o<got;o+=16){
        P("%016llX  ",(unsigned long long)(addr+o));
        for(int i=0;i<16;i++){ if(o+i<got) P("%02X ",b[o+i]); else P("   "); }
        P(" ");
        for(int i=0;i<16;i++){ if(o+i<got){ uint8_t c=b[o+i]; P("%c",(c>=32&&c<127)?c:'.'); } }
        P("\n");
    }
}

// Backscan for the vftable of the object an address lives in.
static void wsObj(uint64_t addr, size_t maxback){
    P("backscan from %016llX, up to %llu bytes\n",
      (unsigned long long)addr,(unsigned long long)maxback);
    for(size_t back=0; back<=maxback; back+=8){
        uint64_t a=addr-back;
        uint64_t vft=0;
        if(!rv::rd(a,&vft,8)) continue;
        if(!rv::inMod(vft)) continue;
        char nm[320];
        if(!rv::rttiName(vft,nm,sizeof(nm))) continue;
        P("  +%-6llu  object %016llX  vft %016llX  %s\n",
          (unsigned long long)back,(unsigned long long)a,(unsigned long long)vft,nm);
        if(back==0) continue;
    }
}

static void wsRtti(uint64_t a){
    uint64_t vft=a;
    char nm[320]; rv::COL c{};
    if(!rv::rttiName(vft,nm,sizeof(nm),&c)){
        // Not a vftable: treat it as an object and read its first qword.
        if(!rv::rd(a,&vft,8) || !rv::rttiName(vft,nm,sizeof(nm),&c)){
            P("no RTTI at %016llX\n",(unsigned long long)a); return;
        }
        P("object %016llX -> vft %016llX\n",(unsigned long long)a,(unsigned long long)vft);
    }
    P("class      %s\n",nm);
    P("vft        MODULE+%llX   subobject offset %u\n",
      (unsigned long long)(vft-rv::g_base), c.offset);
    rv::CHD h{};
    if(rv::mrd(rv::g_base+c.pCHD,&h,sizeof(h)) && h.numBase && h.numBase<256){
        std::vector<uint32_t> rvas(h.numBase);
        if(rv::mrd(rv::g_base+h.pBCA,rvas.data(),4ull*h.numBase)){
            P("bases      %u\n",h.numBase);
            for(uint32_t i=0;i<h.numBase;i++){
                rv::BCD b{};
                if(!rv::mrd(rv::g_base+rvas[i],&b,sizeof(b))) continue;
                char raw[320]{}, bn[320]{};
                if(!rv::mrd(rv::g_base+b.pTD+0x10,raw,sizeof(raw)-1)) continue;
                rv::demangle(raw,bn,sizeof(bn));
                P("   +%-6d %s\n",(int)b.mdisp,bn);
            }
        }
    }
}

static void wsTree(uint64_t inst, int depth, int indent){
    std::string nm; rv::nameOf(inst,nm);
    const std::string& cl=rv::classOf(inst);
    std::vector<uint64_t> kids;
    rv::childrenOf(inst,kids);
    P("%*s%-28s [%s]  %016llX  %d children\n",indent,"",
      nm.empty()?"(unnamed)":nm.c_str(), cl.c_str(),
      (unsigned long long)inst,(int)kids.size());
    if(depth<=0) return;
    for(size_t i=0;i<kids.size() && i<400;i++) wsTree(kids[i],depth-1,indent+2);
}

static void wsFindName(uint64_t root, const char* want, int depth){
    struct Node { uint64_t a; int d; };
    std::vector<Node> q; q.push_back({root,0});
    size_t head=0; int hits=0, seen=0;
    while(head<q.size() && seen<400000){
        Node n=q[head++]; seen++;
        std::string nm;
        if(rv::nameOf(n.a,nm) && !nm.empty() && strstr(nm.c_str(),want)){
            P("  %-30s [%s]  %016llX  depth %d\n",
              nm.c_str(), rv::classOf(n.a).c_str(),(unsigned long long)n.a,n.d);
            if(++hits>=200) break;
        }
        if(n.d>=depth) continue;
        std::vector<uint64_t> kids;
        rv::childrenOf(n.a,kids);
        for(size_t i=0;i<kids.size();i++) q.push_back({kids[i],n.d+1});
    }
    P("%d hits from %d nodes\n",hits,seen);
}

static void wsAttrs(uint64_t inst){
    uint64_t vec=0, be[2]={0,0};
    if(!rv::rd(inst+rv::I_STATEVEC,&vec,8) || vec<0x10000){ P("no side-state vector\n"); return; }
    if(!rv::rd(vec,be,16) || be[1]<be[0]){ P("bad side-state vector\n"); return; }
    size_t span=(size_t)(be[1]-be[0]);
    if(span>0x4000){ P("side-state vector too large (%llu)\n",(unsigned long long)span); return; }
    std::vector<uint8_t> raw(span);
    if(!rv::rd(be[0],raw.data(),span)){ P("side-state unreadable\n"); return; }
    uint64_t cont=0;
    for(size_t o=0;o+rv::SV_ELEM<=span;o+=rv::SV_ELEM){
        uint64_t p; uint16_t kind;
        memcpy(&p,raw.data()+o,8); memcpy(&kind,raw.data()+o+8,2);
        if(kind==rv::SV_KIND_ATTR && p>0x10000){ cont=p; break; }
    }
    if(!cont){ P("instance has no attribute container\n"); return; }
    uint32_t count=0; uint64_t entries=0;
    if(!rv::rd(cont+rv::AC_COUNT,&count,4) || !rv::rd(cont+rv::AC_ENTRIES,&entries,8)){
        P("attribute container unreadable\n"); return; }
    if(!count || count>512 || entries<0x10000){ P("%u attributes\n",count); return; }
    std::vector<uint8_t> buf((size_t)count*rv::AE_STRIDE);
    if(!rv::rd(entries,buf.data(),buf.size())){ P("attribute entries unreadable\n"); return; }
    P("%u attributes on %016llX\n",count,(unsigned long long)inst);
    for(uint32_t i=0;i<count;i++){
        const uint8_t* e=buf.data()+(size_t)i*rv::AE_STRIDE;
        uint64_t rec,type;
        memcpy(&rec,e+rv::AE_NAME,8); memcpy(&type,e+rv::AE_TYPE,8);
        const std::string& nm=rv::attrNameOf(rec);
        std::string ty=rv::inMod(type)?rv::typeNameOf(type):std::string("?");
        P("  %-32s %-10s ",nm.c_str(),ty.c_str());
        if(ty=="string"){
            uint64_t len; memcpy(&len,e+rv::AE_VALUE+0x10,8);
            std::string v;
            if(len<=15) v.assign((const char*)(e+rv::AE_VALUE),(size_t)(len>256?0:len));
            else{
                uint64_t heap; memcpy(&heap,e+rv::AE_VALUE,8);
                if(len<4096 && heap>0x10000){ std::vector<char> t((size_t)len);
                    if(rv::rd(heap,t.data(),(size_t)len)) v.assign(t.data(),(size_t)len); }
            }
            P("\"");
            for(size_t k=0;k<v.size();k++) P("%c",(unsigned char)v[k]>=32&&(unsigned char)v[k]<127?v[k]:'.');
            P("\"  (");
            for(size_t k=0;k<v.size();k++) P("%02X ",(unsigned char)v[k]);
            P(")\n");
        }else if(ty=="double"){
            double d; memcpy(&d,e+rv::AE_VALUE,8); P("%.6f\n",d);
        }else if(ty=="bool"){
            P("%s\n", e[rv::AE_VALUE]?"true":"false");
        }else{
            uint64_t q; memcpy(&q,e+rv::AE_VALUE,8);
            P("raw %016llX\n",(unsigned long long)q);
        }
    }
}

// BFS the pointer graph for a qword VALUE, printing the offset chain that
// reaches it. This is the tool that cracked the attribute layout (HANDOFF 9.4b):
// plant an 8-character attribute value so the needle is exactly one qword, then
// ask where it lives relative to the instance.
static void wsPath(uint64_t start, uint64_t want, int depth, size_t win){
    struct N { uint64_t a; int d; std::string chain; };
    std::vector<N> q; q.push_back({start,0,std::string()});
    std::set<uint64_t> seen; seen.insert(start);
    size_t head=0; int hits=0, visited=0;
    std::vector<uint8_t> buf(win);
    while(head<q.size() && visited<400000 && hits<40){
        N n=q[head++]; visited++;
        SIZE_T got=0;
        // Partial reads are kept: a window that runs off the end of a region
        // still yields what it can, and getting this wrong once cost a whole
        // pass of false negatives.
        ReadProcessMemory(rv::g_h,(LPCVOID)n.a,buf.data(),win,&got);
        if(!got) continue;
        for(size_t o=0;o+8<=got;o+=8){
            uint64_t v; memcpy(&v,buf.data()+o,8);
            char step[32]; sprintf(step," -> +0x%llX",(unsigned long long)o);
            if(v==want){
                P("  HIT  %s%s   at %016llX\n",n.chain.c_str(),step,(unsigned long long)(n.a+o));
                if(++hits>=40) break;
                continue;
            }
            if(n.d>=depth) continue;
            if(v<0x10000 || (v&7)) continue;
            if(seen.count(v)) continue;
            seen.insert(v);
            q.push_back({v,n.d+1,n.chain+step});
        }
    }
    P("%d hits, %d nodes visited\n",hits,visited);
}

// The DataModel, found the same way the overlay finds it. Published by
// MemSource::bootstrap, so this just needs a bootstrap to have happened.
static int workshop(std::vector<std::string>& a){
    if(!rv::attach()){ P("cannot attach to RobloxPlayerBeta.exe\n"); return 2; }
    std::string m = a.empty()? "info" : a[0];
    std::vector<std::string> r(a.begin()+(a.empty()?0:1), a.end());

    if(m=="info"){
        auto rg=rv::regions();
        uint64_t total=0; for(auto& x:rg) total+=x.size;
        P("pid        %lu\n",(unsigned long)rv::g_pid);
        P("module     %016llX  %.1f MB\n",
          (unsigned long long)rv::g_base, rv::g_size/1048576.0);
        P("regions    %d committed readable, %.1f MB\n",(int)rg.size(),total/1048576.0);
        return 0;
    }
    if(m=="read" && r.size()>=2){ wsHexDump(Hex(r[0].c_str()),(size_t)Num(r[1].c_str())); return 0; }
    if(m=="obj"  && r.size()>=1){ wsObj(Hex(r[0].c_str()), r.size()>1?(size_t)Num(r[1].c_str()):0x400); return 0; }
    if(m=="rtti" && r.size()>=1){ rv::loadImage(); wsRtti(Hex(r[0].c_str())); return 0; }
    if(m=="attrs"&& r.size()>=1){ wsAttrs(Hex(r[0].c_str())); return 0; }
    if(m=="tree" && r.size()>=1){ wsTree(Hex(r[0].c_str()), r.size()>1?(int)Num(r[1].c_str()):1, 0); return 0; }
    if(m=="findname" && r.size()>=2){ wsFindName(Hex(r[0].c_str()), r[1].c_str(),
                                                 r.size()>2?(int)Num(r[2].c_str()):6); return 0; }
    if(m=="path" && r.size()>=2){ wsPath(Hex(r[0].c_str()),Hex(r[1].c_str()),
                                         r.size()>2?(int)Num(r[2].c_str()):3, 0x400); return 0; }
    if(m=="cmp" && r.size()>=1){
        std::vector<uint64_t> t;
        for(size_t i=0;i<r.size();i++) t.push_back(Hex(r[i].c_str()));
        const size_t SZ=0x180;
        std::vector<std::vector<uint8_t>> b(t.size(), std::vector<uint8_t>(SZ,0));
        for(size_t i=0;i<t.size();i++) rv::rd(t[i],b[i].data(),SZ);
        P("slot-by-slot over %d objects, 0x%zX bytes\n",(int)t.size(),SZ);
        for(size_t o=0;o+8<=SZ;o+=8){
            uint64_t v0; memcpy(&v0,b[0].data()+o,8);
            bool same=true;
            for(size_t i=1;i<t.size();i++){ uint64_t v; memcpy(&v,b[i].data()+o,8); if(v!=v0){ same=false; break; } }
            P("  +0x%03llX  %-5s",(unsigned long long)o, same?"SAME":"DIFF");
            for(size_t i=0;i<t.size();i++){ uint64_t v; memcpy(&v,b[i].data()+o,8); P(" %016llX",(unsigned long long)v); }
            if(same && rv::inMod(v0)){ char nm[320]; if(rv::rttiName(v0,nm,sizeof(nm))) P("   %s",nm); }
            P("\n");
        }
        return 0;
    }
    if(m=="needle" && !r.empty()){
        std::vector<std::vector<uint8_t>> pats;
        for(size_t i=0;i<r.size();i++){
            std::vector<uint8_t> p;
            const char* s=r[i].c_str();
            if(s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
            for(size_t k=0;s[k] && s[k+1];k+=2){
                char h[3]={s[k],s[k+1],0};
                p.push_back((uint8_t)strtoul(h,nullptr,16));
            }
            if(!p.empty()) pats.push_back(p);
        }
        if(pats.empty()){ P("no usable patterns\n"); return 2; }
        int hits=0;
        sweep([&](uint64_t base,const uint8_t* d,size_t n){
            for(size_t pi=0;pi<pats.size();pi++){
                const std::vector<uint8_t>& p=pats[pi];
                if(n<p.size()) continue;
                for(size_t o=0;o+p.size()<=n;o++){
                    if(d[o]!=p[0]) continue;
                    if(memcmp(d+o,p.data(),p.size())) continue;
                    if(hits<4000) P("  pattern %d at %016llX\n",(int)pi,(unsigned long long)(base+o));
                    hits++;
                }
            }
        });
        P("%d hits\n",hits);
        return 0;
    }
    if((m=="ptr"||m=="ptrin") && !r.empty()){
        uint64_t lo=Hex(r[0].c_str()), hi=lo+8;
        if(m=="ptrin" && r.size()>=2) hi=lo+Num(r[1].c_str());
        std::vector<uint64_t> extra;
        if(m=="ptr") for(size_t i=1;i<r.size();i++) extra.push_back(Hex(r[i].c_str()));
        int hits=0;
        sweep([&](uint64_t base,const uint8_t* d,size_t n){
            for(size_t o=0;o+8<=n;o+=8){
                uint64_t v; memcpy(&v,d+o,8);
                bool hit = (m=="ptrin") ? (v>=lo && v<hi) : (v==lo);
                if(!hit && m=="ptr") for(size_t i=0;i<extra.size();i++) if(v==extra[i]){ hit=true; break; }
                if(!hit) continue;
                uint64_t at=base+o;
                if(hits<3000){
                    P("  %016llX -> %016llX",(unsigned long long)at,(unsigned long long)v);
                    // backscan a little for a vftable so the holder is named
                    for(size_t back=0; back<=0x200; back+=8){
                        uint64_t vft=0;
                        if(!rv::rd(at-back,&vft,8) || !rv::inMod(vft)) continue;
                        char nm[320];
                        if(rv::rttiName(vft,nm,sizeof(nm))){ P("   [%s +0x%llX]",nm,(unsigned long long)back); break; }
                    }
                    P("\n");
                }
                hits++;
            }
        });
        P("%d hits\n",hits);
        return 0;
    }
    if(m=="classes"){
        rv::loadImage();
        const char* want = r.empty()? nullptr : r[0].c_str();
        int n=0;
        for(size_t o=0;o+16<=rv::g_img.size(); o+=8){
            uint64_t v; memcpy(&v,rv::g_img.data()+o,8);
            if(!rv::inMod(v)) continue;
            uint64_t vft=rv::g_base+o+8;
            char nm[320]; rv::COL c{};
            if(!rv::rttiName(vft,nm,sizeof(nm),&c)) continue;
            if(want && !strstr(nm,want)) continue;
            P("%016llX  MODULE+%08llX  sub %3u  %s\n",
              (unsigned long long)vft,(unsigned long long)(vft-rv::g_base),c.offset,nm);
            n++;
        }
        P("%d classes\n",n);
        return 0;
    }
    if(m=="vfscan" && !r.empty()){
        uint64_t vft=Hex(r[0].c_str());
        int hits=0;
        sweep([&](uint64_t base,const uint8_t* d,size_t n){
            for(size_t o=0;o+8<=n;o+=8){
                uint64_t v; memcpy(&v,d+o,8);
                if(v!=vft) continue;
                if(hits<3000) P("  object %016llX\n",(unsigned long long)(base+o));
                hits++;
            }
        });
        P("%d objects\n",hits);
        return 0;
    }
    if(m=="dm"){
        MemSource ms; Frame fr;
        for(int i=0;i<30 && !g_dmAddr; i++){ ms.poll(fr); if(!g_dmAddr) Sleep(100); }
        if(!g_dmAddr){ P("DataModel not found - is Rivals in a place?\n"); return 2; }
        P("DataModel  %016llX\nWorkspace  %016llX\nCamera     %016llX\n\n",
          (unsigned long long)g_dmAddr,(unsigned long long)g_wsAddr,
          (unsigned long long)g_camAddr);
        wsTree(g_dmAddr, r.empty()?1:(int)Num(r[0].c_str()), 0);
        return 0;
    }
    P("unknown scan mode '%s'\n",m.c_str());
    return 2;
}
// ---------------------------------------------------------------------------
static void splitArgs(const std::string& cmd, std::vector<std::string>& out){
    std::string cur;
    for(size_t i=0;i<cmd.size();i++){
        char c=cmd[i];
        if(c==' '||c=='\t'){ if(!cur.empty()){ out.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if(!cur.empty()) out.push_back(cur);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR lpCmd,int){
    // DPI-aware so a scaled (125/150%) display doesn't shift the overlay off the
    // game. Resolved at runtime so it compiles and runs on any Windows: on
    // Win10 1703+ it asks for per-monitor-v2, and older systems fall back to
    // system-DPI-aware, and anything that rejects both just runs as-is.
    typedef BOOL(WINAPI*SpdacFn)(HANDLE);
    SpdacFn spdac=(SpdacFn)GetProcAddress(GetModuleHandleA("user32.dll"),"SetProcessDpiAwarenessContext");
    if(spdac){
        HANDLE pmv2=(HANDLE)(-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if(!spdac(pmv2)){
            typedef BOOL(WINAPI*SpdaFn)(void);
            SpdaFn spda=(SpdaFn)GetProcAddress(GetModuleHandleA("user32.dll"),"SetProcessDPIAware");
            if(spda) spda();
        }
    } else {
        typedef BOOL(WINAPI*SpdaFn)(void);
        SpdaFn spda=(SpdaFn)GetProcAddress(GetModuleHandleA("user32.dll"),"SetProcessDPIAware");
        if(spda) spda();
    }
    initFont();
    cfgLoad();                       // remembered toggles + the cached DataModel RVA
    std::vector<std::string> args;
    splitArgs(lpCmd?lpCmd:"", args);
    std::string mode = args.empty()? "" : args[0];

    // ---------------- the RE workshop ----------------
    if(mode=="scan"){
        outOpen("overlay_log.txt");
        std::vector<std::string> rest(args.begin()+1,args.end());
        int rc=workshop(rest);
        outClose();
        return rc;
    }

    // ---------------- headless diagnostics ----------------
    // Every one of these writes overlay_log.txt. They exist because the lesson
    // this project keeps re-learning is that a measurement beats a hypothesis
    // every single time (HANDOFF 10.7).
    if(mode=="trace"){
        int secs = args.size()>1 ? (int)Num(args[1].c_str()) : 15;
        if(secs<1||secs>300) secs=15;
        g_trace=true;
        outOpen("overlay_log.txt");
        MemSource ms; Frame fr;
        P("trace %d s - one line per CHANGE of health or draw-state\n\n",secs);
        struct Prev { float hp; std::string drop; };
        std::unordered_map<std::string,Prev> prev;
        DWORD t0=GetTickCount(); int frames=0, polls=0;
        while((int)((GetTickCount()-t0)/1000) < secs){
            g_traceRows.clear();
            bool ok=ms.poll(fr);
            frames++; if(ok) polls++;
            if(!ok){ Sleep(40); continue; }
            double t=(GetTickCount()-t0)/1000.0;
            {   static int pc=-1, pd=-1;
                int drawn=(int)fr.ents.size();
                if(pc!=g_traceCharsRead || pd!=drawn){
                    P("%6.2fs   rescan: %d ws kids -> notModel %d, childrenRead FAILED %d, noHumanoid %d, noPrimitive %d, kept %d, RESCUED %d, expired %d\n",
                      t, g_rsKids, g_rsNotModel, g_rsChildFail, g_rsNoHum, g_rsNoPrim, g_rsKept,
                      g_rsRescued, g_rsExpired);
                    P("%6.2fs   [chars %d -> %d, drawn %d -> %d]\n", t, pc, g_traceCharsRead, pd, drawn);
                    pc=g_traceCharsRead; pd=drawn;
                }
            }
            for(size_t i=0;i<g_traceRows.size();i++){
                TraceRow& r=g_traceRows[i];
                auto it=prev.find(r.name);
                bool changed = (it==prev.end()) ||
                               fabsf(it->second.hp-r.hp)>0.01f ||
                               it->second.drop!=r.drop;
                if(!changed) continue;
                char tid[16]="--", eid[16]="--";
                if(!r.team.empty()) sprintf(tid,"%02X",(unsigned char)r.team[0]);
                if(!r.env.empty())  sprintf(eid,"%02X",(unsigned char)r.env[0]);
                P("%6.2fs %-20s hp %7.2f/%-7.2f T%s E%s %s -> %s\n",
                  t, r.name.c_str(), r.hp, r.mhp, tid, eid,
                  it==prev.end()?"(new)":it->second.drop.c_str(), r.drop);
                prev[r.name]={r.hp,r.drop};
            }
            Sleep(40);
        }
        {
            char ltid[16]="unset", leid[16]="unset";
            if(!g_myTeam.empty()) sprintf(ltid,"%02X",(unsigned char)g_myTeam[0]);
            if(!g_myEnv.empty())  sprintf(leid,"%02X",(unsigned char)g_myEnv[0]);
            P("\n--- %d polls of %d frames ok, local TeamID %s EnvironmentID %s, %d chars\n",
              polls, frames, ltid, leid, g_traceCharsRead);
            P("WORKSPACE WALK: %d passes, %d FAILED (%.1f%%)\n",
              g_rsPasses, g_rsWsWalkFail, g_rsPasses?100.0*g_rsWsWalkFail/g_rsPasses:0.0);
        }
        outClose();
        return 0;
    }

    if(mode=="aimtest"){
        int secs = args.size()>1 ? (int)Num(args[1].c_str()) : 8;
        if(secs<1||secs>120) secs=8;
        for(size_t i=1;i<args.size();i++) if(args[i]=="dry") g_aimDry=true;
        outOpen("overlay_log.txt");
        g_opt.aim=true; g_opt.aimMode=0;                 // always-on for the duration
        MemSource ms; Frame fr;
        P("aimtest %ds  %s   part=%s  fov=%s %dpx  smoothing=%d\n\n",
          secs, g_aimDry?"DRY (no input sent)":"LIVE (moves the mouse)",
          g_opt.aimPart?"torso":"head", g_opt.aimFov?"on":"off",
          g_opt.aimFovPx, g_opt.aimSmooth);
        DWORD t0=GetTickCount(); int frames=0, engaged=0;
        std::string lastT; float minErr=1e9f;
        while((int)((GetTickCount()-t0)/1000)<secs){
            if(!ms.poll(fr)){ Sleep(30); continue; }
            frames++;
            int cx,cy,cw,ch; robloxClient(cx,cy,cw,ch);
            bool fg = g_rbxWnd && GetForegroundWindow()==g_rbxWnd;
            aimTick(fr, fg || g_aimDry);
            double t=(GetTickCount()-t0)/1000.0;
            if(g_aim.engaged){
                engaged++;
                if(g_aim.errDeg<minErr) minErr=g_aim.errDeg;
                P("%5.2fs  %-20s err %7.3f deg  cmd %5d,%-5d  gain %+.4f/%+.4f  ents %d\n",
                  t, g_aim.targetName.c_str(), g_aim.errDeg, g_aim.cmdX, g_aim.cmdY,
                  g_aim.gainX, g_aim.gainY, (int)fr.ents.size());
                lastT=g_aim.targetName;
            }else if(frames%20==0){
                P("%5.2fs  idle  (foreground %d, ents %d, candidates %d, nearest %.0f px, fov %d px)\n",
                  t, fg?1:0, (int)fr.ents.size(), g_aim.candN, g_aim.nearestPx,
                  g_opt.aimFov?g_opt.aimFovPx:-1);
            }
            Sleep(22);
        }
        P("\n--- %d frames, %d engaged, best error %.3f deg, last target %s\n",
          frames, engaged, engaged?minErr:-1.0, lastT.empty()?"(none)":lastT.c_str());
        P("final gain %.4f / %.4f deg per mouse count, %d calibration windows\n",
          g_aim.gainX, g_aim.gainY, g_aim.calN);
        outClose();
        return 0;
    }

    if(mode=="wallinfo"){
        outOpen("overlay_log.txt");
        g_opt.aim=true; g_opt.aimWall=true;
        MemSource ms; Frame fr; bool ok=false;
        for(int i=0;i<40 && !ok;i++){ ok=ms.poll(fr); if(!ok) Sleep(150); }
        for(int i=0;i<400 && (ms.gRunning||g_geom.empty()); i++) ms.poll(fr);
        if(!ok){ P("MemSource bootstrap FAILED\n"); outClose(); return 2; }
        P("camera   %.2f %.2f %.2f\n",fr.cam.pos.x,fr.cam.pos.y,fr.cam.pos.z);
        P("geometry %d boxes from %d nodes, radius %.0f studs\n\n",
          g_geomParts,g_geomNodes,g_geomRadius);
        int inside=0;
        for(size_t i=0;i<g_geom.size();i++) if(insideOBB(g_geom[i],fr.cam.pos)) inside++;
        P("boxes CONTAINING the camera: %d   (any >0 used to block every ray)\n\n",inside);
        P("per enemy:\n");
        for(size_t i=0;i<fr.ents.size();i++){
            const Entity& e=fr.ents[i];
            for(int part=0;part<2;part++){
                Vec3 pt=aimPointFor(e,part);
                g_wallLastIdx=-1;
                bool occ=occluded(fr.cam.pos,pt);
                Vec3 dd=sub(pt,fr.cam.pos);
                float dist=sqrtf(dot(dd,dd));
                P("  %-20s %-5s dist %7.1f  %s",
                  e.name.c_str(), part?"body":"head", dist, occ?"BLOCKED":"clear  ");
                if(occ && g_wallLastIdx>=0){
                    const rv::OBB& b=g_geom[g_wallLastIdx];
                    P("  by box %d at %.1f %.1f %.1f  size %.1fx%.1fx%.1f  %.0f studs along",
                      g_wallLastIdx,b.c.x,b.c.y,b.c.z,b.hx*2,b.hy*2,b.hz*2,g_wallLastT);
                }
                P("\n");
            }
        }
        std::vector<int> idx(g_geom.size());
        for(size_t i=0;i<idx.size();i++) idx[i]=(int)i;
        std::sort(idx.begin(),idx.end(),[](int a,int b){ return g_geom[a].r>g_geom[b].r; });
        P("\n10 largest boxes:\n");
        for(size_t i=0;i<idx.size() && i<10;i++){
            const rv::OBB& b=g_geom[idx[i]];
            Vec3 dd=sub(b.c,fr.cam.pos);
            P("  size %8.1f x %8.1f x %8.1f  centre %9.1f %9.1f %9.1f  %6.0f studs away\n",
              b.hx*2,b.hy*2,b.hz*2,b.c.x,b.c.y,b.c.z,sqrtf(dot(dd,dd)));
        }
        outClose();
        return 0;
    }

    if(mode=="memdump"){
        outOpen("overlay_log.txt");
        MemSource ms; Frame fr; bool ok=false;
        for(int i=0;i<40 && !ok;i++){ ok=ms.poll(fr); if(!ok) Sleep(150); }
        if(ok && g_opt.aim && g_opt.aimWall)
            for(int i=0;i<200 && ms.gRunning; i++) ms.poll(fr);
        if(!ok) P("MemSource: bootstrap FAILED (Roblox not running, or no read access)\n");
        else{
            P("SOURCE      memory (no executor)\n");
            P("bootstrap   %.2fs = attach %.2f + vftable %.2f (%s) + object hunt %.2f + tree %.2f\n",
              g_bootAttach+g_bootVft+g_bootObj+g_bootTree,
              g_bootAttach, g_bootVft, g_bootCached?"cached RVA":"full sweep",
              g_bootObj, g_bootTree);
            P("            object hunt read %.0f MB\n", g_bootBytes/1048576.0);
            P("cam pos     %.4f %.4f %.4f\n",fr.cam.pos.x,fr.cam.pos.y,fr.cam.pos.z);
            P("cam right   %.6f %.6f %.6f\n",fr.cam.right.x,fr.cam.right.y,fr.cam.right.z);
            P("cam up      %.6f %.6f %.6f\n",fr.cam.up.x,fr.cam.up.y,fr.cam.up.z);
            P("cam look    %.6f %.6f %.6f\n",fr.cam.look.x,fr.cam.look.y,fr.cam.look.z);
            P("fov         %.4f deg   viewport %dx%d\n",fr.cam.fovDeg,fr.cam.w,fr.cam.h);
            {
                char ltid[24]="unset", leid[24]="unset";
                if(!g_myTeam.empty()) sprintf(ltid,"%02X",(unsigned char)g_myTeam[0]);
                if(!g_myEnv.empty())  sprintf(leid,"%02X",(unsigned char)g_myEnv[0]);
                P("local       TeamID %s  EnvironmentID %s\n", ltid, leid);
            }
            P("characters  %d read, %d teammates hidden, %d kept (arena fallback radius %.0f, Y band %.0f)\n",
              g_memSeen, g_memMates, g_memArena, ARENA_RADIUS, ARENA_Y_BAND);
            P("walls       %d parts cached from %d nodes (radius %.0f studs)\n",
              g_geomParts, g_geomNodes, g_geomRadius);
            P("entities    %d\n",(int)fr.ents.size());
            for(size_t i=0;i<fr.ents.size();i++){
                Entity& e=fr.ents[i];
                float sx,sy,dp; bool on=worldToScreen(fr.cam,e.head,sx,sy,dp);
                char tid[24]="--", eid[24]="--";
                if(!e.team.empty()) sprintf(tid,"%02X",(unsigned char)e.team[0]);
                if(!e.env.empty())  sprintf(eid,"%02X",(unsigned char)e.env[0]);
                const char* vis="";
                if(g_geomParts) vis = e.covHead&&e.covBody ? "[COVERED]  "
                                    : (e.covHead ? "[head hid] " : (e.covBody ? "[body hid] " : "[VISIBLE]  "));
                P("%s",vis);
                P("E %-22s T%s E%s hp %6.1f/%-6.1f head %9.3f %9.3f %9.3f  bot %9.3f %9.3f %9.3f  %s",
                  e.name.c_str(),tid,eid,e.health,e.maxHealth,
                  e.head.x,e.head.y,e.head.z,e.root.x,e.root.y,e.root.z,
                  on?"screen ":"BEHIND ");
                if(on) P("%7.1f %7.1f  d=%.1f",sx,sy,dp);
                P("\n");
            }
        }
        outClose();
        return ok?0:2;
    }

    if(mode=="help" || mode=="-h" || mode=="/?"){
        outOpen("overlay_log.txt");
        P("overlay.exe                 live overlay. INSERT menu, F7 log, END quit\n"
          "overlay.exe memdump         camera + every player + screen coords\n"
          "overlay.exe aimtest N [dry] run the aim loop headless for N seconds\n"
          "overlay.exe wallinfo        what the wall check can and cannot see\n"
          "overlay.exe trace N         per-character draw-state changes for N seconds\n"
          "overlay.exe camprobe [write]  can MAGIC BULLET be silent on this build?\n"
          "overlay.exe scan <mode> ..  the RE workshop (scan info for a start)\n");
        outClose();
        return 0;
    }

    // ---------------- the live overlay ----------------
    // ONE EXE, RUN IT, DONE. No arguments, no order of operations, no waiting for
    // the right moment. It comes up immediately, tells you what it is doing, and
    // attaches by itself the instant Rivals is readable - before the game is
    // launched, after it, or across a rejoin. Nothing here blocks: the old
    // version spent up to three seconds in a bootstrap loop before it drew a
    // single pixel, so launching it too early looked like it had failed.

    // One instance only. Two overlays means two sets of ESP boxes drawn one on
    // top of the other and two processes reading the same memory, and the second
    // one is always an accident.
    HANDLE only=CreateMutexA(NULL,TRUE,"RivalsExternalOverlay");
    if(only && GetLastError()==ERROR_ALREADY_EXISTS){
        HWND prev=FindWindowA("RvOverlay",NULL);
        if(prev) FlashWindow(prev,TRUE);
        return 0;
    }

    // Size the surface to the whole primary screen so it can host any client
    // size; the layered window is resized to the drawn area every frame anyway.
    int wx=0, wy=0;
    int ww=GetSystemMetrics(SM_CXSCREEN), wh=GetSystemMetrics(SM_CYSCREEN);
    if(ww<640) ww=1920;
    if(wh<480) wh=1080;

    WNDCLASSEXA wc={sizeof(wc)}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName="RvOverlay";
    RegisterClassExA(&wc);
    g_hwnd=CreateWindowExA(
        WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        "RvOverlay","rv",WS_POPUP, wx,wy,ww,wh, NULL,NULL,hInst,NULL);
    makeSurface(ww,wh);
    ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);

    // Paint the card BEFORE the first poll. Bootstrap blocks the thread for a
    // second or two while it hunts the DataModel through a couple of gigabytes
    // of heap, and until this was here that was a second or two of the exe being
    // running and looking like it had done nothing.
    beginFrame();
    drawStatus(g_fb, false, 0);
    present(wx,wy);
    endFrame();

    MemSource* src=new MemSource();
    bool live=false;
    DWORD attachedAt=0;      // for the "attached" card, which then fades out
    int   cw=ww, chh=wh;     // last known client size

    Frame frame; g_live=false;
    g_trace=true; g_startTk=GetTickCount();
    LARGE_INTEGER freq,last,now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    int fcount=0; double acc=0; MSG msg;
    double accPoll=0, accDraw=0, accPres=0;
    bool menu=false, prevIns=false, prevLb=false;
    // Default Windows timer granularity is ~15.6 ms, so a Sleep(11) to fill out a
    // 60 Hz frame actually sleeps a whole tick or two and lands nearer 32 fps.
    // Ask for 1 ms so the frame limiter hits the rate it is aiming at.
    timeBeginPeriod(1);
    const double FRAME_TARGET = 1.0/45.0;
    LARGE_INTEGER wStart;
    while(g_running){
        QueryPerformanceCounter(&wStart);
        while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT) g_running=false; TranslateMessage(&msg); DispatchMessage(&msg); }
        if(GetAsyncKeyState(VK_END)&1) g_running=false;       // END quits (ESC is used in-game)
        // F7: write out the rolling flight recorder. Press it AFTER seeing a box
        // blink out - the ring already holds the seconds leading up to it.
        if(GetAsyncKeyState(VK_F7)&1){ traceDump("overlay_log.txt","F7 pressed in game"); g_f7Flash=GetTickCount(); }
        bool ins=(GetAsyncKeyState(VK_INSERT)&0x8000)!=0;
        if(ins && !prevIns){
            menu=!menu;
            LONG_PTR ex=GetWindowLongPtr(g_hwnd,GWL_EXSTYLE);
            if(menu) ex &= ~(LONG_PTR)WS_EX_TRANSPARENT; else ex |= (LONG_PTR)WS_EX_TRANSPARENT;
            SetWindowLongPtr(g_hwnd,GWL_EXSTYLE,ex);
            if(!menu){ g_uiCapture=0; if(g_cfgDirty){ cfgSave(); g_cfgDirty=false; } }
        }
        prevIns=ins;

        if(g_uiCapture==1){
            // MOUSE1 is bindable, so the click that OPENED the capture box would
            // otherwise be read as the key being bound. Drain every edge latch
            // once and only start listening on the following frame.
            for(int vk=1; vk<255; vk++) GetAsyncKeyState(vk);
            g_uiCapture=2;
        }else if(g_uiCapture==2){
            for(int vk=1; vk<255; vk++){
                if(vk==VK_INSERT||vk==VK_END||vk==VK_F7||vk==VK_F8) continue;
                if(!(GetAsyncKeyState(vk)&1)) continue;
                if(vk==VK_ESCAPE){ g_uiCapture=0; break; }
                if(g_uiCaptureTgt){ *g_uiCaptureTgt=vk; g_cfgDirty=true; }
                g_uiCapture=0; break;
            }
        }

        LARGE_INTEGER tA,tB,tC,tD; QueryPerformanceCounter(&tA);
        g_traceRows.clear();
        bool ok=src->poll(frame);
        // Attach and detach on their own. `ready` going false is how MemSource
        // reports a rejoin or the game closing, and the next poll rebuilds
        // everything from scratch - so this needs no special case for either.
        if(ok && !live){ live=true;  g_live=true;  g_srcTag="MEM";  attachedAt=GetTickCount(); }
        if(!ok && live){ live=false; g_live=false; g_srcTag="---";  attachedAt=0; g_entCount=0; }
        if(live){ int cx,cy,cwn,chn; if(robloxClient(cx,cy,cwn,chn)){ wx=cx; wy=cy; cw=cwn; chh=chn; } }
        traceRecord((GetTickCount()-g_startTk)/1000.0);
        QueryPerformanceCounter(&tB);
        if(live && frame.cam.w>0 && (frame.cam.w!=g_fb.w || frame.cam.h!=g_fb.h)){
            makeSurface(frame.cam.w,frame.cam.h);
            g_prevX1=-1;                       // the old dirty box means nothing now
        }
        g_entCount=0; if(live) for(auto& e:frame.ents) if(e.health>0) g_entCount++;

        // ---- aimbot ----
        // Both gated on Rivals being the foreground window and the menu being
        // closed. The gate covers the BIND as well as the movement: in toggle
        // mode a right-click in Chrome would otherwise flip the aimbot on behind
        // his back.
        bool fg = live && g_rbxWnd && GetForegroundWindow()==g_rbxWnd;
        if(live){
            bool held = fg && (GetAsyncKeyState(g_opt.aimKey)&0x8000)!=0;
            bool want = false;
            if(g_opt.aim){
                if(g_opt.aimMode==0)      want = true;                 // always
                else if(g_opt.aimMode==1) want = held;                 // hold
                else{                                                  // toggle
                    if(held && !g_aim.prevKey) g_aim.toggleOn=!g_aim.toggleOn;
                    want = g_aim.toggleOn;
                }
            }
            g_aim.prevKey=held;
            aimTick(frame, want && fg && !menu);
            if(g_aim.engaged) aimLogWrite();
        }
        beginFrame();
        if(live){
            drawEsp(frame);
            aimDraw(g_fb, frame);
        }
        drawStatus(g_fb, live, attachedAt);
        QueryPerformanceCounter(&tC);
        if(g_f7Flash && GetTickCount()-g_f7Flash < 2000)
            drawTextC(g_fb, g_fb.w/2, 40, "overlay_log.txt written", ARGB(255,120,230,140));
        if(g_autoFlash && GetTickCount()-g_autoFlash < 3000)
            drawTextC(g_fb, g_fb.w/2, 58, g_autoWhy, ARGB(255,255,190,90));
        if(menu){
            POINT cur; GetCursorPos(&cur); int mx=cur.x-wx, my=cur.y-wy;
            bool lb=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
            bool clickEdge = lb && !prevLb; prevLb=lb;
            drawMenu(g_fb, mx, my, clickEdge, lb);
        } else { prevLb=false; g_uiDrag=-1; }
        present(wx,wy);
        endFrame();
        QueryPerformanceCounter(&tD);
        accPoll+=(double)(tB.QuadPart-tA.QuadPart)/freq.QuadPart;
        accDraw+=(double)(tC.QuadPart-tB.QuadPart)/freq.QuadPart;
        accPres+=(double)(tD.QuadPart-tC.QuadPart)/freq.QuadPart;

        QueryPerformanceCounter(&now);
        double dt=(double)(now.QuadPart-last.QuadPart)/freq.QuadPart; last=now; acc+=dt; fcount++;
        if(acc>=2.0){
            // Twice a second was pointless churn on a file nobody reads while
            // playing; the numbers it carries are second-scale anyway.
            g_fpsShown=(int)(fcount/acc+0.5);
            FILE* pf=fopen("overlay_perf.txt","w");
            if(pf){
                fprintf(pf,"src=%s fps=%d ents=%d chars_read=%d chars_arena=%d\n",
                        g_srcTag,g_fpsShown,g_entCount,g_memSeen,g_memArena);
                fprintf(pf,"aim: geom=%d parts (%d nodes)  wallTests=%d blocked=%d  gain=%.3f/%.3f  zoom=%d/%d cal=%d\n",
                        g_geomParts, g_geomNodes, g_wallTests, g_wallBlocked,
                        g_aim.gainX, g_aim.gainY, g_aim.gbIdx+1, g_gbN, g_aim.calN);
                g_wallTests=g_wallBlocked=0;
                fprintf(pf,"per frame ms:  poll=%.2f  draw=%.2f  present=%.2f  total=%.2f\n",
                        accPoll*1000.0/fcount, accDraw*1000.0/fcount,
                        accPres*1000.0/fcount,
                        (accPoll+accDraw+accPres)*1000.0/fcount);
                fprintf(pf,"present rect:  %dx%d px of %dx%d\n",
                        g_presentW,g_presentH,g_fb.w,g_fb.h);
                fclose(pf);
            }
            fcount=0; acc=0; accPoll=accDraw=accPres=0;
        }

        {
            double work=(double)(now.QuadPart-wStart.QuadPart)/freq.QuadPart;
            if(work < FRAME_TARGET){
                DWORD ms=(DWORD)((FRAME_TARGET-work)*1000.0+0.5);
                if(ms) Sleep(ms);
            }
        }
    }
    timeEndPeriod(1);
    if(g_liveLog){ fclose(g_liveLog); g_liveLog=nullptr; }
    if(g_cfgDirty){ cfgSave(); g_cfgDirty=false; }
    delete src;
    return 0;
}
