// NorseAnimSwap - replace animations by condition (UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood), 2026-11-18.
// Modelled on Open Animation Replacer (Skyrim): a rule names the animation it replaces, the condition under which it
// does (NorseConditions, about the unit playing it), and a priority; when the game starts an animation, the highest-
// priority rule whose condition holds wins, and one of its replacements is played instead. Nothing in the paks changes.
//
// Where: every montage start in Norse ends in one virtual on the AnimInstance, vtable slot 95 (+0x2f8),
// Montage_PlayInternal(Montage, BlendSettings, PlayRate, ReturnType, StartAt, bStopAll) - mapped by NorsePayload. Most of
// Norse's animations (attacks, abilities, reactions) are animation sequences played as a dynamic montage made on the
// spot; a few are montage assets (AM_*). So the swap is done before the original runs:
//  - a montage asset replaced by a montage: the montage argument is swapped;
//  - a dynamic montage replaced by a sequence: the sequence inside it is swapped (its AnimReference, AnimEndTime and the
//    montage's SequenceLength) - the game keeps the same montage object, so whatever it waits on still ends, and the
//    new animation's own notifies drive the timing.
//
// anims\*.ini (NorseManifest order), one [section] per rule; its name is only a label:
//   [Brutes swing heavier]
//   Replace = A_Outcast_Attack_01* -> /Game/MyPack/A_Heavy_01.A_Heavy_01 | /Game/MyPack/A_Heavy_02.A_Heavy_02
//             ; left: the montage or its first animation (name, * wildcards); right: one or more replacements (| = a
//             ; random pick each time); repeatable
//   When = UnitType(Outcast_Brute*)    ; optional, NorseConditions, about the unit playing it
//   Priority = 100                     ; higher wins (OAR); default 0
//   Chance = 100                       ; rolled each time
// Console: anim.log on|off (log every animation start: unit, montage, first animation), anim.list, anim.reload.
#include "NorseReflect.hpp"
#include "NorseManifest.hpp"
#include "NorseFilters.hpp"
#include "MinHook.h"

#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace nrn;
using namespace nrn::ue;
using namespace norse;

namespace
{
    fs::path g_mod;

    fs::path ModDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModDir), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(buf).parent_path().parent_path();
    }

    std::wstring PathOfObj(UObject* o)
    {
        std::wstring full = o->GetFullName(nullptr);
        return full.substr(full.find(L' ') + 1);
    }

    struct Target
    {
        std::wstring path;
        UObject* obj = nullptr;
        int32_t idx = -1;
        bool montage = false;
    };
    struct Line
    {
        std::wstring from; // lower-case pattern
        std::vector<Target> to;
    };
    struct Rule
    {
        std::string name, where;
        std::vector<Line> lines;
        std::wstring when;
        int cond = 0, priority = 0, chance = 100;
        bool condTried = false;
    };
    std::vector<Rule> g_rules;
    bool g_log = false;

    using NcCompileFn = int (*)(const char*, char*, int);
    using NcEvalFn = int (*)(int, void*, void*);
    NcCompileFn g_ncCompile = nullptr;
    NcEvalFn g_ncEval = nullptr;

    template <typename F>
    F ModExport(const wchar_t* mod, const char* name)
    {
        HMODULE m = GetModuleHandleW((g_mod.parent_path() / mod / L"dlls" / L"main.dll").c_str());
        return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
    }

    void CompileConditions()
    {
        if (!g_ncCompile) g_ncCompile = ModExport<NcCompileFn>(L"NorseConditions", "NC_Compile");
        if (!g_ncEval) g_ncEval = ModExport<NcEvalFn>(L"NorseConditions", "NC_Eval");
        for (Rule& r : g_rules)
        {
            if (r.when.empty() || r.cond || r.condTried) continue;
            if (!g_ncCompile)
            {
                NRN_WARN("[%s] When = %s, but NorseConditions is not loaded - the rule is off until it is", r.name.c_str(), Narrow(r.when).c_str());
                continue;
            }
            char err[256]{};
            r.condTried = true;
            r.cond = g_ncCompile(Narrow(r.when).c_str(), err, sizeof(err));
            if (!r.cond) NRN_ERROR("[%s] When does not compile: %s - the rule is off", r.name.c_str(), err);
        }
    }

    void LoadRules()
    {
        g_rules.clear();
        fs::path dir = g_mod / "anims";
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            for (const auto& f : fs::directory_iterator(dir, ec))
                if (f.is_regular_file() && Lower(f.path().extension().wstring()) == L".ini") files.push_back(f.path());
        nm::Manifest man;
        man.Resolve(g_mod.parent_path().parent_path());
        files = man.Arrange(files, [](const std::string& s) { NRN_INFO("load order: %s", s.c_str()); });
        size_t lines = 0;
        for (const auto& f : files)
        {
            Rule* cur = nullptr;
            int n = 0;
            for (const auto& raw : ReadLines(f))
            {
                ++n;
                std::wstring l = Trim(StripComment(raw));
                if (l.empty()) continue;
                std::string where = Narrow(f.filename().wstring()) + ":" + std::to_string(n);
                if (l.front() == L'[' && l.back() == L']')
                {
                    g_rules.push_back({});
                    cur = &g_rules.back();
                    cur->name = Narrow(Trim(l.substr(1, l.size() - 2)));
                    cur->where = where;
                    continue;
                }
                size_t eq = l.find(L'=');
                if (!cur || eq == std::wstring::npos)
                {
                    NRN_WARN("%s: not a [rule] or key = value line - skipped", where.c_str());
                    continue;
                }
                std::wstring k = Lower(Trim(l.substr(0, eq))), v = Trim(l.substr(eq + 1));
                if (k == L"replace")
                {
                    size_t arrow = v.find(L"->");
                    if (arrow == std::wstring::npos)
                    {
                        NRN_WARN("%s: Replace needs 'animation -> new' - skipped", where.c_str());
                        continue;
                    }
                    Line ln;
                    ln.from = Lower(Trim(v.substr(0, arrow)));
                    for (const auto& t : SplitList(v.substr(arrow + 2), L'|'))
                        if (!t.empty()) ln.to.push_back({t});
                    if (ln.to.empty()) NRN_WARN("%s: Replace has no replacement - skipped", where.c_str());
                    else
                    {
                        cur->lines.push_back(ln);
                        ++lines;
                    }
                }
                else if (k == L"when") cur->when = v;
                else if (k == L"priority") cur->priority = _wtoi(v.c_str());
                else if (k == L"chance") cur->chance = ParseChance(v);
                else NRN_WARN("%s: unknown key '%s' (Replace, When, Priority, Chance)", where.c_str(), Narrow(k).c_str());
            }
        }
        // highest priority first; equal priority keeps load order
        std::stable_sort(g_rules.begin(), g_rules.end(), [](const Rule& a, const Rule& b) { return a.priority > b.priority; });
        // Conditions are compiled at map load (and by anim.reload), never here: this mod loads before NorseConditions
        // (alphabetical), whose conditions are not registered yet - 'IsEnemy' read as unknown and the rule stayed off.
        NRN_INFO("anim rules: %zu rule(s), %zu Replace line(s) from %zu file(s)", g_rules.size(), lines, files.size());
    }

    bool IsA(UObject* o, const wchar_t* cls)
    {
        for (UStruct* c = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(o)); c; c = c->GetInheritanceSuper())
            if (_wcsicmp(ShortName(PathOfObj(reinterpret_cast<UObject*>(c))).c_str(), cls) == 0) return true;
        return false;
    }

    // Load a replacement (again, when a map change freed it - the root set does not hold across one)
    bool Ready(Target& t)
    {
        if (t.obj && Alive(t.obj, t.idx)) return true;
        t.obj = LoadKept(t.path.c_str());
        if (!t.obj)
        {
            WarnOnce("anim:" + Narrow(t.path), "%s: not found - that replacement is skipped", Narrow(t.path).c_str());
            return false;
        }
        t.idx = IndexOf(t.obj);
        t.montage = IsA(t.obj, L"AnimMontage");
        return true;
    }

    // ------------------------------------------------------------------ the montage's own animation
    FProperty* TracksProp(UObject* montage)
    {
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(montage));
        return cls ? FindProp(cls, L"SlotAnimTracks") : nullptr;
    }
    // The first segment's AnimReference value, as exported: "/Script/Engine.AnimSequence'/Game/...'"
    std::wstring FirstAnimation(const std::wstring& tracks, size_t* at = nullptr, size_t* len = nullptr)
    {
        size_t a = tracks.find(L"AnimReference=");
        if (a == std::wstring::npos) return {};
        a += 14;
        size_t b = tracks.find_first_of(L",)", a);
        if (at) *at = a;
        if (len) *len = (b == std::wstring::npos ? tracks.size() : b) - a;
        std::wstring ref = tracks.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        ref.erase(std::remove(ref.begin(), ref.end(), L'"'), ref.end());
        size_t q = ref.find(L'\'');
        if (q != std::wstring::npos) ref = ref.substr(q + 1, ref.rfind(L'\'') - q - 1);
        return ref;
    }
    float PlayLength(UObject* anim)
    {
        Buf b;
        if (Reflect::Get().ParmsSizeOf(anim, L"GetPlayLength") != 4 || !Reflect::Get().Call(anim, L"GetPlayLength", b.b, 4, sizeof(b.b))) return 0;
        float f = 0;
        std::memcpy(&f, b.b, 4);
        return f;
    }
    int Segments(const std::wstring& tracks)
    {
        int n = 0;
        for (size_t p = tracks.find(L"AnimReference="); p != std::wstring::npos; p = tracks.find(L"AnimReference=", p + 1)) ++n;
        return n;
    }

    // Put another sequence into a dynamic (one-segment) montage before it plays
    bool SwapSequence(UObject* montage, const std::wstring& tracks, UObject* seq)
    {
        FProperty* p = TracksProp(montage);
        float newLen = PlayLength(seq);
        if (!p || newLen <= 0) return false;
        size_t at = 0, len = 0;
        if (FirstAnimation(tracks, &at, &len).empty()) return false;
        std::wstring t = tracks.substr(0, at) + L"\"/Script/Engine.AnimSequence'" + PathOfObj(seq) + L"'\"" + tracks.substr(at + len);
        size_t e = t.find(L"AnimEndTime=");
        if (e != std::wstring::npos)
        {
            size_t ee = t.find_first_of(L",)", e);
            wchar_t num[32];
            swprintf_s(num, L"%.6f", newLen);
            t = t.substr(0, e + 12) + num + t.substr(ee);
        }
        bool faulted = false;
        void* addr = reinterpret_cast<uint8_t*>(montage) + p->GetOffset_Internal();
        if (!SehImport(p, t.c_str(), addr, montage, &faulted) || faulted) return false;
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(montage));
        if (FProperty* sl = FindProp(cls, L"SequenceLength")) std::memcpy(reinterpret_cast<uint8_t*>(montage) + sl->GetOffset_Internal(), &newLen, 4);
        return Lower(FirstAnimation(Export(p, addr, montage))) == Lower(PathOfObj(seq));
    }

    UObject* Owner(UObject* anim)
    {
        Buf b;
        if (!Reflect::Get().Call(anim, L"GetOwningActor", b.b, 8, sizeof(b.b))) return nullptr;
        UObject* o = nullptr;
        std::memcpy(&o, b.b, 8);
        return o;
    }

    int g_swaps = 0;

    // Returns the montage to play (the same one, edited, or another)
    UObject* Choose(UObject* anim, UObject* montage)
    {
        if (!montage) return montage;
        FProperty* tp = TracksProp(montage);
        std::wstring tracks = tp ? Export(tp, reinterpret_cast<uint8_t*>(montage) + tp->GetOffset_Internal(), montage) : L"";
        std::wstring mname = ShortName(PathOfObj(montage)), aname = ShortName(FirstAnimation(tracks));
        UObject* unit = Owner(anim);
        if (g_log)
            NRN_INFO("start: %s | montage %s | animation %s", unit ? Narrow(ShortName(PathOfObj(unit))).c_str() : "-", Narrow(mname).c_str(),
                     Narrow(aname).c_str());
        if (g_rules.empty()) return montage;
        std::wstring lm = Lower(mname), la = Lower(aname);
        bool dynamic = Segments(tracks) == 1 && montage->GetFullName(nullptr).find(L"/Engine/Transient") != std::wstring::npos;
        static std::mt19937 rng{std::random_device{}()};
        for (Rule& r : g_rules)
            for (Line& ln : r.lines)
            {
                if (!Glob(ln.from.c_str(), lm.c_str()) && (la.empty() || !Glob(ln.from.c_str(), la.c_str()))) continue;
                if (!r.when.empty() && (!r.cond || !g_ncEval || !g_ncEval(r.cond, unit, nullptr))) continue;
                if (r.chance < 100 && static_cast<int>(rng() % 100) >= r.chance) continue;
                Target& t = ln.to[ln.to.size() == 1 ? 0 : rng() % ln.to.size()];
                if (!Ready(t)) continue;
                std::string who = unit ? Narrow(ShortName(PathOfObj(unit))) : "-";
                if (t.montage)
                {
                    ++g_swaps;
                    NRN_INFO("%s: %s -> %s (%s, priority %d)", who.c_str(), Narrow(aname.empty() ? mname : aname).c_str(),
                             Narrow(ShortName(t.path)).c_str(), r.name.c_str(), r.priority);
                    return t.obj;
                }
                if (!dynamic && Segments(tracks) != 1)
                {
                    WarnOnce("multi:" + Narrow(mname), "%s: a montage asset with several animations - replace it with a montage, not a sequence",
                             Narrow(mname).c_str());
                    continue;
                }
                if (SwapSequence(montage, tracks, t.obj))
                {
                    ++g_swaps;
                    NRN_INFO("%s: %s -> %s (%s, priority %d)", who.c_str(), Narrow(aname).c_str(), Narrow(ShortName(t.path)).c_str(), r.name.c_str(),
                             r.priority);
                }
                else NRN_WARN("%s: putting %s into %s failed - played as it was", who.c_str(), Narrow(ShortName(t.path)).c_str(), Narrow(mname).c_str());
                return montage;
            }
        return montage;
    }

    // ------------------------------------------------------------------ the hook (AnimInstance vtable slot 95)
    using PlayInternalFn = float(__fastcall*)(UObject* anim, UObject* montage, const void* blend, float rate, uint8_t retType, float startAt,
                                              bool stopAll);
    constexpr size_t kPlayInternalSlot = 0x2f8 / 8;
    std::map<void*, PlayInternalFn> g_originals;

    float __fastcall PlayInternalHook(UObject* anim, UObject* montage, const void* blend, float rate, uint8_t retType, float startAt, bool stopAll)
    {
        void* fn = (*reinterpret_cast<void***>(anim))[kPlayInternalSlot];
        auto it = g_originals.find(fn);
        if (it == g_originals.end()) return 0.f;
        UObject* play = montage;
        try
        {
            play = Choose(anim, montage);
        }
        catch (...)
        {
            NRN_ERROR("animation choice threw - played as it was");
            play = montage;
        }
        return it->second(anim, play, blend, rate, retType, startAt, stopAll);
    }

    void HookVtableOf(UObject* anim, const char* from)
    {
        if (!anim) return;
        void* fn = (*reinterpret_cast<void***>(anim))[kPlayInternalSlot];
        if (!fn || g_originals.count(fn)) return;
        PlayInternalFn orig = nullptr;
        if (MH_CreateHook(fn, reinterpret_cast<void*>(&PlayInternalHook), reinterpret_cast<void**>(&orig)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        {
            NRN_ERROR("Montage_PlayInternal (vtable slot %zu of %s) at %p: hook failed", kPlayInternalSlot, from, fn);
            return;
        }
        g_originals[fn] = orig;
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        NRN_INFO("Montage_PlayInternal hooked: vtable slot %zu of %s -> base+0x%llx", kPlayInternalSlot, from,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fn) - base));
    }

    void HookAll()
    {
        HookVtableOf(FindObj(L"/Script/Engine.Default__AnimInstance"), "AnimInstance");
        std::vector<UObject*> all;
        try
        {
            RC::Unreal::UObjectGlobals::FindAllOf(L"AnimInstance", all);
        }
        catch (...)
        {
        }
        for (UObject* a : all) HookVtableOf(a, Narrow(ShortName(PathOfObj(a))).c_str());
    }

    bool RunCommand(const std::wstring& c)
    {
        std::wstring cmd = Lower(Trim(c));
        NRN_INFO("command: %s", Narrow(cmd).c_str());
        if (cmd == L"anim.log on" || cmd == L"anim.log off")
        {
            g_log = cmd == L"anim.log on";
            NRN_INFO("animation starts %s logged", g_log ? "are" : "are not");
        }
        else if (cmd == L"anim.reload")
        {
            LoadRules();
            CompileConditions();
            HookAll();
        }
        else if (cmd == L"anim.list")
        {
            for (const Rule& r : g_rules)
            {
                NRN_INFO("  [%s] priority %d when '%s' chance %d", r.name.c_str(), r.priority, Narrow(r.when).c_str(), r.chance);
                for (const Line& l : r.lines) NRN_INFO("    %s -> %zu replacement(s)", Narrow(l.from).c_str(), l.to.size());
            }
            NRN_INFO("anim.list: %zu rule(s), %d swap(s) so far", g_rules.size(), g_swaps);
        }
        else NRN_WARN("unknown command - anim.log on|off | anim.list | anim.reload");
        return true;
    }
} // namespace

class NorseAnimSwap : public RC::CppUserModBase
{
  public:
    NorseAnimSwap()
    {
        ModName = L"NorseAnimSwap";
        ModVersion = L"" NAS_VERSION;
        ModDescription = L"Replace animations by condition and priority, like Open Animation Replacer.";
        ModAuthors = L"ApocryphaRealm";
        for (auto& s : g_nopVtbl) s = reinterpret_cast<void*>(&NopFn);
        g_errOut.vtbl = g_nopVtbl;
        g_mod = ModDir();
        std::wstring dir;
        int logLevel = 2; // uLogLevel: 0 trace, 1 debug, 2 info, 3 warn, 4 error, 6 off
        for (const auto& raw : ReadLines(g_mod / "NorseAnimSwap.ini"))
        {
            std::wstring l = Trim(raw);
            if (l.rfind(L"sLogDir=", 0) == 0) dir = Trim(l.substr(8));
            if (l.rfind(L"uLogLevel=", 0) == 0) logLevel = _wtoi(l.c_str() + 10);
        }
        Log::Get().Open((dir.empty() ? g_mod : fs::path(dir)) / "NorseAnimSwap.log", logLevel);
        Log::Get().Write(Level::Error, "NorseAnimSwap v%s - log level %d", NAS_VERSION, logLevel);
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) NRN_ERROR("MinHook did not start (%d)", static_cast<int>(st));
        LoadRules();
    }

    auto on_unreal_init() -> void override
    {
        try
        {
            RC::Unreal::Hook::RegisterInitGameStatePostCallback([](RC::Unreal::AGameModeBase*) {
                CompileConditions();
                try
                {
                    HookAll();
                }
                catch (...)
                {
                    NRN_ERROR("hooking threw");
                }
                if (g_swaps) NRN_INFO("last map: %d animation(s) replaced", g_swaps);
                g_swaps = 0;
            });
            RC::Unreal::Hook::RegisterProcessConsoleExecCallback(
                    [](UObject*, const wchar_t* cmd, RC::Unreal::FOutputDevice&, UObject*) -> bool {
                        if (!cmd || _wcsnicmp(cmd, L"anim.", 5) != 0) return false;
                        static ULONGLONG last = 0;
                        static std::wstring lastCmd;
                        ULONGLONG now = GetTickCount64();
                        if (lastCmd == cmd && now - last < 250) return true;
                        last = now;
                        lastCmd = cmd;
                        return RunCommand(cmd);
                    });
        }
        catch (const std::exception& e)
        {
            NRN_ERROR("callback registration failed: %s", e.what());
        }
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorseAnimSwap();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
