// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gazake
// =============================================================================
//  GNClicker - autoclicker transparente, codigo aberto
// -----------------------------------------------------------------------------
//  O que este programa FAZ:
//    - Envia cliques do mouse (esquerdo/direito) numa taxa configuravel (CPS)
//      enquanto voce segura o botao do mouse (depois de armar por uma tecla).
//    - Macros: blockhit, jitter, whitelist de slots, throwpot e W-tap.
//    - Opcionalmente so clica quando uma janela especifica esta em foco.
//    - Jitter (so no clique esquerdo): so age quando a janela ativa parece ser o Minecraft ou um
//      launcher/client conhecido. Para isso le o TITULO e o NOME DO .EXE da janela ativa (nada mais).
//    - Nunca clica enquanto a propria janela do GNClicker esta ativa.
//    - Bolinha verde: esconde a janela na bandeja do sistema (continua funcionando).
//    - Toca um som de clique e guarda a configuracao em %APPDATA%\GNClicker.
//
//  O que este programa NAO FAZ (de proposito):
//    - Nao acessa a memoria de outros processos (sem OpenProcess/WriteProcessMemory).
//    - Nao faz conexoes de rede, nao baixa nada, nao envia nada.
//    - Nao cria persistencia (nao mexe em registro/Run, nem em tarefas agendadas).
//    - Nao se auto-deleta, nao roda comandos de shell e nao apaga rastros.
//
//  Tudo que ele usa sao APIs padrao do Windows:
//    mouse_event / SendInput (clicar e teclas), GetAsyncKeyState (so as teclas
//    configuradas), um hook de mouse (so estado fisico dos botoes e scroll),
//    GetCursorInfo, GetForegroundWindow + titulo/nome do .exe da janela ativa (so para saber se e o
//    Minecraft), Shell_NotifyIcon (icone da bandeja) e PlaySound. Nao ha hook de teclado.
//
//  Framework de interface: Dear ImGui + Direct3D 11 (baixado pelo build.bat).
//  O scaffolding de janela/DX11 abaixo segue o exemplo oficial do Dear ImGui (MIT).
// =============================================================================

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <windows.h>
#include <tchar.h>

#include <atomic>
#include <thread>
#include <string>
#include <chrono>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <vector>

#include "logo_data.h"    // pixels da logo (imagem fornecida pelo usuario)
#include "sounds_data.h"  // sons de clique originais (sintetizados)
#include "click_sounds.h" // sons de clique embutidos no exe
#include <mmsystem.h>     // PlaySound
#include <dwmapi.h>       // cantos arredondados e sombra da janela sem moldura

// ---- Direct3D globals (do exemplo oficial do ImGui) -------------------------
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static ID3D11ShaderResourceView* g_logoSRV = nullptr;   // textura da logo

bool CreateDeviceD3D(HWND hWnd);
void CreateLogoTexture();
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =============================================================================
//  Estado compartilhado do clicker (lido pelas threads de clique)
// =============================================================================
struct Clicker {
    std::atomic<bool> enabled { false };   // clicker habilitado
    std::atomic<int>  cps     { 10 };      // cliques por segundo alvo
    std::atomic<int>  key     { 0 };       // VK do botao que, SEGURADO, dispara o clique
    std::atomic<int>  armKey  { 0 };       // tecla que ARMA/DESARMA este lado (configuravel)
    std::atomic<bool> armed   { false };   // estado ligado/desligado deste lado
    bool left = true;                      // true=botao esquerdo, false=direito
};

static Clicker               g_left;       // botao esquerdo
static Clicker               g_right;      // botao direito
static std::atomic<bool>     g_running     { true };
static std::atomic<bool>     g_randomize   { true };   // jitter humano no CPS
static std::atomic<bool>     g_restrict    { false };  // so clicar na janela alvo
static std::atomic<uintptr_t> g_targetHwnd { 0 };
static std::string           g_targetTitle;   // vazio = nenhuma janela capturada ainda

// Idioma da interface: 0 = English (padrao), 1 = Portugues. L(en, pt) escolhe o texto.
static int g_lang = 0;
static inline const char* L(const char* en, const char* pt) { return g_lang == 1 ? pt : en; }

// ---- macros e filtros (blockhit, jitter, slots, throwpot, W-tap) ---------------
static std::atomic<bool> g_inventory   { true };   // clicar com o cursor visivel (inventario/menus)
static std::atomic<bool> g_blockhit    { false };  // bloqueia (RMB) junto com cada clique esquerdo
static std::atomic<bool> g_jitter      { false };  // pequenos movimentos de mouse ao clicar
static std::atomic<int>  g_jitterSpeed { 2 };      // amplitude do jitter em pixels (1..10)
static std::atomic<bool> g_slotFilter  { false };  // whitelist de slots da hotbar
static std::atomic<bool> g_slotAllowed[9];         // slots em que o clicker pode agir
static std::atomic<int>  g_curSlot     { 0 };      // slot atual (0..8), estimado por teclas/scroll
static std::atomic<int>  g_hotbarVK[9];            // VK de cada slot (padrao 1..9)
static std::atomic<int>  g_throwKey    { 0 };      // tecla do throwpot
static std::atomic<int>  g_throwSlot   { 1 };      // slot (0..8) da pocao
static std::atomic<int>  g_throwDelay  { 50 };     // ms entre os passos
static std::atomic<bool> g_throwReturn { true };   // volta ao slot anterior depois
static std::atomic<bool> g_wtap        { false };  // W-tap (solta o W por instantes ao atacar)
static std::atomic<int>  g_wtapDelay   { 30 };     // ms com o W solto
static std::atomic<bool> g_wtapOnlyAuto{ true };   // so durante o autoclick
static std::atomic<bool> g_wtapRequest { false };
static std::atomic<bool> g_rainbow     { false };  // tema arco-iris
static std::atomic<int>  g_clickMode   { 0 };      // 0 = segurar o botao do mouse (padrao); 1 = alternar (a tecla liga/desliga o clique continuo)
static std::atomic<bool> g_mcFocus     { false };  // a janela ativa parece ser o Minecraft (atualizado por uma thread)
static HWND              g_hwnd        = nullptr;  // janela principal do programa

// ---- deteccao do Minecraft (usada so pelo Jitter; o auto click funciona em qualquer programa) ----
// Olha apenas o TITULO e o NOME DO .EXE da janela ativa. Nao le a memoria de nenhum programa.
// BEGIN_MC_DETECT
static std::wstring lowerCopy(const std::wstring& s) {
    std::wstring r = s;
    for (auto& ch : r) if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
    return r;
}
// title = titulo da janela, exe = nome do arquivo do processo (ambos podem ser vazios)
static bool looksLikeMinecraft(const std::wstring& titleIn, const std::wstring& exeIn) {
    // nomes do jogo e de launchers/clients famosos
    static const wchar_t* kNames[] = {
        L"minecraft", L"lunar client", L"lunarclient", L"badlion", L"feather client", L"feather launcher",
        L"labymod", L"tlauncher", L"sklauncher", L"prism launcher", L"prismlauncher", L"multimc", L"polymc",
        L"atlauncher", L"gdlauncher", L"curseforge", L"technic launcher", L"modrinth", L"ftb app"
    };
    // navegadores e afins que so tem "minecraft" no titulo (ex.: aba de wiki) nunca contam
    static const wchar_t* kNotGame[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"opera_gx.exe", L"brave.exe", L"vivaldi.exe",
        L"iexplore.exe", L"explorer.exe", L"notepad.exe", L"code.exe", L"discord.exe", L"whatsapp.exe", L"telegram.exe"
    };
    std::wstring title = lowerCopy(titleIn), exe = lowerCopy(exeIn);
    for (const wchar_t* b : kNotGame) if (exe == b) return false;
    for (const wchar_t* n : kNames) if (title.find(n) != std::wstring::npos || exe.find(n) != std::wstring::npos) return true;
    return false;
}
// END_MC_DETECT

// Sons de clique: embutidos (originais) + quaisquer .wav que o usuario coloque
// na pasta "sounds" ao lado do programa. g_sound = -1 significa "nenhum".
struct Snd { std::string name; const unsigned char* data; int len; };
static std::vector<std::vector<unsigned char>> g_customStorage;  // bytes dos .wav da pasta
static std::vector<std::string>                g_customNames;
static std::vector<Snd>                        g_sndList;        // lista final (embutidos + custom)
static std::atomic<int> g_sound { -1 };

static std::string exeDir() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    size_t pos = p.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
}

static void loadCustomSounds() {
    std::string dir = exeDir() + "\\sounds";
    CreateDirectoryA(dir.c_str(), nullptr);   // cria a pasta se nao existir
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.wav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string fn = fd.cFileName;
        FILE* f = fopen((dir + "\\" + fn).c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 44 && sz < 5 * 1024 * 1024) {       // WAV valido e ate 5MB
            std::vector<unsigned char> buf((size_t)sz);
            if (fread(buf.data(), 1, (size_t)sz, f) == (size_t)sz) {
                g_customStorage.push_back(std::move(buf));
                g_customNames.push_back(fn.substr(0, fn.find_last_of('.')));
            }
        }
        fclose(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void buildSoundList() {
    g_sndList.clear();
    // Sons embutidos de mouse (click_sounds.h) -> aparecem primeiro.
    for (int i = 0; i < g_clickSoundsCount; ++i)
        g_sndList.push_back({ g_clickSounds[i].name, g_clickSounds[i].data, g_clickSounds[i].len });
    // Sons sintetizados.
    g_sndList.push_back({ "Click", snd0, snd0_len });
    g_sndList.push_back({ "Tick",  snd1, snd1_len });
    g_sndList.push_back({ "Pop",   snd2, snd2_len });
    g_sndList.push_back({ "Mouse", snd3, snd3_len });
    // Extras que o usuario colocar na pasta "sounds" (ignora nomes ja embutidos).
    for (size_t i = 0; i < g_customStorage.size(); ++i) {
        bool dup = false;
        for (const auto& s : g_sndList) if (s.name == g_customNames[i]) { dup = true; break; }
        if (!dup)
            g_sndList.push_back({ g_customNames[i], g_customStorage[i].data(), (int)g_customStorage[i].size() });
    }
}

static void playClickSound() {
    int si = g_sound.load();
    if (si >= 0 && si < (int)g_sndList.size())
        PlaySoundA((LPCSTR)g_sndList[si].data, nullptr, SND_MEMORY | SND_ASYNC);   // flags 5 = memoria + assincrono
}

static std::string currentSoundName() {
    int si = g_sound.load();
    return (si >= 0 && si < (int)g_sndList.size()) ? g_sndList[si].name : std::string();
}

static void setSoundByName(const std::string& name) {
    for (size_t i = 0; i < g_sndList.size(); ++i)
        if (g_sndList[i].name == name) { g_sound.store((int)i); return; }
    g_sound.store(-1);
}

// Inicializa botoes + PADRAO: ambos os lados usam a tecla R para ativar
// (o usuario pode trocar cada um separadamente, ou deixar os dois iguais).
static void initClickers() {
    g_left.left = true;  g_right.left = false;
    g_left.enabled.store(true);
    g_left.key.store(VK_LBUTTON);   // segurar o botao esquerdo dispara
    g_left.armKey.store('R');
    g_left.cps.store(12);
    g_right.enabled.store(true);
    g_right.key.store(VK_RBUTTON);  // segurar o botao direito dispara
    g_right.armKey.store('R');
    g_right.cps.store(12);
    for (int i = 0; i < 9; ++i) {
        g_hotbarVK[i].store('1' + i);     // padrao do Minecraft: teclas 1..9
        g_slotAllowed[i].store(true);
    }
}

// ---- binds da hotbar do Minecraft (le %APPDATA%\.minecraft\options.txt) ------
// Somente leitura, e so de um arquivo de config do jogo. Aceita o formato
// antigo (codigo numerico do LWJGL = scancode) e o novo ("key.keyboard.x").
static int vkFromOptionValue(const std::string& v) {
    if (v.empty()) return 0;
    if (isdigit((unsigned char)v[0])) {                       // 1.8: "key_key.hotbar.1:2"
        int sc = atoi(v.c_str());
        return sc > 0 ? (int)MapVirtualKeyA((UINT)sc, MAPVK_VSC_TO_VK) : 0;
    }
    const std::string p = "key.keyboard.";
    if (v.compare(0, p.size(), p) != 0) return 0;             // 1.13+: "key.keyboard.1"
    std::string n = v.substr(p.size());
    if (n.size() == 1 && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1])) {
        int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + (f - 1);
    }
    if (n.compare(0, 7, "keypad.") == 0 && n.size() == 8 && isdigit((unsigned char)n[7]))
        return VK_NUMPAD0 + (n[7] - '0');
    return 0;
}

static bool fetchMinecraftBinds() {
    const char* ap = getenv("APPDATA");
    if (!ap) return false;
    FILE* f = fopen((std::string(ap) + "\\.minecraft\\options.txt").c_str(), "r");
    if (!f) return false;
    char line[256];
    bool any = false;
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        const std::string pre = "key_key.hotbar.";
        if (s.compare(0, pre.size(), pre) != 0) continue;
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        int slot = atoi(s.c_str() + pre.size()) - 1;
        std::string val = s.substr(colon + 1);
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' ')) val.pop_back();
        int vk = vkFromOptionValue(val);
        if (slot >= 0 && slot < 9 && vk) { g_hotbarVK[slot].store(vk); any = true; }
    }
    fclose(f);
    return any;
}

// ---- helpers ----------------------------------------------------------------
static inline bool keyDown(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Estado FISICO dos botoes do mouse (atualizado pelo hook, ignorando cliques
// injetados pelo proprio programa). Necessario para "segurar esquerdo e clicar
// esquerdo": a leitura simples se cancelaria sozinha com os cliques injetados.
static std::atomic<bool> g_physLeft   { false };
static std::atomic<bool> g_physRight  { false };
static std::atomic<bool> g_physMiddle { false };
static std::atomic<bool> g_physX1     { false };
static std::atomic<bool> g_physX2     { false };
static HHOOK             g_mouseHook = nullptr;
static std::atomic<unsigned long> g_hookThreadId { 0 };

// Botoes do mouse: usa o estado FISICO do hook. Teclado: leitura normal.
static bool inputDown(int vk) {
    switch (vk) {
        case VK_LBUTTON:  return g_physLeft.load();
        case VK_RBUTTON:  return g_physRight.load();
        case VK_MBUTTON:  return g_physMiddle.load();
        case VK_XBUTTON1: return g_physX1.load();
        case VK_XBUTTON2: return g_physX2.load();
        default:          return keyDown(vk);
    }
}

// Hook global de mouse: so registra eventos FISICOS (seus), nunca os injetados.
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
        if (!(ms->flags & LLMHF_INJECTED)) {
            if (wParam == WM_MOUSEWHEEL) {   // scroll troca o slot da hotbar
                int d = (short)HIWORD(ms->mouseData);
                int s = g_curSlot.load() + (d > 0 ? -1 : 1);
                g_curSlot.store(((s % 9) + 9) % 9);
            }
            switch (wParam) {
                case WM_LBUTTONDOWN: g_physLeft.store(true);   break;
                case WM_LBUTTONUP:   g_physLeft.store(false);  break;
                case WM_RBUTTONDOWN: g_physRight.store(true);  break;
                case WM_RBUTTONUP:   g_physRight.store(false); break;
                case WM_MBUTTONDOWN: g_physMiddle.store(true); break;
                case WM_MBUTTONUP:   g_physMiddle.store(false);break;
                case WM_XBUTTONDOWN: { int b=HIWORD(ms->mouseData); if(b==XBUTTON1)g_physX1.store(true); else g_physX2.store(true); } break;
                case WM_XBUTTONUP:   { int b=HIWORD(ms->mouseData); if(b==XBUTTON1)g_physX1.store(false);else g_physX2.store(false);} break;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// Thread DEDICADA do hook de mouse: instala o hook e roda um loop de mensagens
// proprio, atendendo o hook na hora. Assim o movimento do mouse NAO sofre
// atraso pelo render/vsync da janela (era o que deixava o mouse "picotado").
static void mouseHookThread() {
    g_hookThreadId.store((unsigned long)GetCurrentThreadId());
    g_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(nullptr), 0);
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
    if (g_mouseHook) { ::UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
}

// Tecla simulada por scancode (o Minecraft/LWJGL le scancodes).
static void sendKey(int vk, bool up) {
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (!sc) return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &in, sizeof(in));
}
static void tapKey(int vk, int holdMs = 15) {
    sendKey(vk, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    sendKey(vk, true);
}

static bool cursorVisible() {
    CURSORINFO ci = { sizeof(ci) };
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

static bool windowOk() {
    // nunca clica (nem faz jitter) enquanto a janela do proprio GNClicker esta ativa:
    // senao segurar o botao do mouse para usar o app dispararia o auto click nele mesmo
    if (g_hwnd && GetForegroundWindow() == g_hwnd) return false;
    if (g_restrict.load()) {
        HWND target = (HWND)g_targetHwnd.load();
        if (!target || GetForegroundWindow() != target) return false;
    }
    // "Inventory" desligado: so age in-game (cursor oculto), nunca em menus/inventario.
    if (!g_inventory.load() && cursorVisible()) return false;
    // whitelist de slots da hotbar
    if (g_slotFilter.load()) {
        int s = g_curSlot.load();
        if (s < 0 || s > 8 || !g_slotAllowed[s].load()) return false;
    }
    return true;
}

static std::string keyName(int vk) {
    switch (vk) {
        case 0:            return L("(none)", "(nenhuma)");
        case VK_LBUTTON:   return L("Left Mouse", "Mouse Esquerdo");
        case VK_RBUTTON:   return L("Right Mouse", "Mouse Direito");
        case VK_MBUTTON:   return L("Middle Mouse", "Mouse Meio");
        case VK_XBUTTON1:  return "Mouse X1";
        case VK_XBUTTON2:  return "Mouse X2";
    }
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    char buf[64] = {0};
    LONG lparam = (LONG)(sc << 16);
    if (sc != 0 && GetKeyNameTextA(lparam, buf, (int)sizeof(buf)) > 0)
        return std::string(buf);
    char h[16];
    std::snprintf(h, sizeof(h), "VK 0x%02X", vk);
    return std::string(h);
}

// ---- espera precisa -----------------------------------------------------------
// Sleep() no Windows tem granularidade de ~15 ms (e o Windows 11 ignora o timer de 1 ms de programas
// que nao estao em primeiro plano, como este quando o jogo esta na frente). Isso derrubava o CPS real.
// Aqui: timer de ALTA resolucao para quase todo o tempo + contagem fina (QueryPerformanceCounter)
// nos ultimos ~2 ms. Cada thread usa o seu proprio timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static void preciseSleepUs(long long us) {
    if (us <= 0) return;
    LARGE_INTEGER freq, now;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&now);
    const long long target = now.QuadPart + us * freq.QuadPart / 1000000;
    thread_local HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (us > 2500) {
        long long waitUs = us - 2000;
        bool waited = false;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(waitUs * 10);            // 100 ns, negativo = relativo
            if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { ::WaitForSingleObject(timer, 1000); waited = true; }
        }
        if (!waited) ::Sleep((DWORD)(waitUs / 1000));
    }
    do { ::YieldProcessor(); ::QueryPerformanceCounter(&now); } while (now.QuadPart < target);
}

// ---- thread de clique (uma por botao) ---------------------------------------
static void clickWorker(Clicker* c) {
    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    int offX = 0, offY = 0;   // deslocamento atual do jitter (desfeito no proximo clique)
    while (g_running.load()) {
        bool want = false;
        if (c->enabled.load() && c->armed.load())
            // modo 0 (padrao): so clica enquanto o botao do mouse esta segurado
            // modo 1 (Settings): armado = clica continuamente, como um autoclicker comum
            want = (g_clickMode.load() == 1) ? true : inputDown(c->key.load());
        if (want && windowOk()) {
            // intervalo do ciclo (CPS + variacao humana)
            int cps = c->cps.load();
            if (cps < 1)   cps = 1;
            if (cps > 100) cps = 100;
            double ms = 1000.0 / cps;
            if (g_randomize.load()) {
                std::uniform_real_distribution<double> d(0.80, 1.20);
                ms *= d(rng);
            }
            long long halfUs = (long long)(ms * 500.0);

            // Ciclo do clique: segura metade do intervalo, solta,
            // espera a outra metade e SO ENTAO toca o som (som no fim do clique).
            // blockhit: solta o RMB so durante o golpe e volta a bloquear logo depois
            bool bh = c->left && g_blockhit.load() && g_physRight.load();
            if (bh) mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            mouse_event(c->left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            preciseSleepUs(halfUs);
            mouse_event(c->left ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            if (bh) mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            if (c->left) {
                if (g_wtap.load()) g_wtapRequest.store(true);
                // jitter so no Minecraft (ou launcher/client conhecido); o clique em si funciona em qualquer janela
                if (g_jitter.load() && g_mcFocus.load()) {
                    int sp = g_jitterSpeed.load();
                    if (sp < 1) sp = 1; if (sp > 15) sp = 15;
                    std::uniform_int_distribution<int> jd(-sp, sp);
                    int nx = jd(rng), ny = jd(rng);
                    mouse_event(MOUSEEVENTF_MOVE, (DWORD)(nx - offX), (DWORD)(ny - offY), 0, 0);
                    offX = nx; offY = ny;
                }
            }
            preciseSleepUs(halfUs);
            playClickSound();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
}

// ---- thread do hotkey: detecta a tecla de ARMAR (R) o tempo todo -----------
// Roda independente da janela, entao funciona mesmo com o jogo em tela cheia.
static void hotkeyWorker() {
    int prevL = 0, prevR = 0;
    while (g_running.load()) {
        int vkL = g_left.armKey.load();
        bool dL = (vkL > 0) && keyDown(vkL);
        if (dL && !prevL) g_left.armed.store(!g_left.armed.load());
        prevL = dL ? 1 : 0;

        int vkR = g_right.armKey.load();
        bool dR = (vkR > 0) && keyDown(vkR);
        if (dR && !prevR) g_right.armed.store(!g_right.armed.load());
        prevR = dR ? 1 : 0;

        // slot atual da hotbar: pelas teclas dos slots (scroll e' tratado no hook)
        for (int i = 0; i < 9; ++i)
            if (keyDown(g_hotbarVK[i].load())) { g_curSlot.store(i); break; }

        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }
}

// ---- detecta se a janela ativa e o Minecraft (atualiza g_mcFocus) ----------------
// Le so o titulo e o nome do .exe da janela ativa, e so quando ela muda. Nada da memoria dos outros programas.
static void mcDetectWorker() {
    HWND lastFg = nullptr;
    std::wstring lastTitle;
    bool lastResult = false;
    while (g_running.load()) {
        HWND fg = ::GetForegroundWindow();
        bool res = false;
        if (fg && fg != g_hwnd) {
            wchar_t t[256] = {0};
            ::GetWindowTextW(fg, t, 255);
            std::wstring title = t;
            if (fg != lastFg || title != lastTitle) {
                std::wstring exe;
                DWORD pid = 0;
                ::GetWindowThreadProcessId(fg, &pid);
                if (pid) {
                    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (h) {
                        wchar_t p[MAX_PATH] = {0};
                        DWORD n = MAX_PATH;
                        if (::QueryFullProcessImageNameW(h, 0, p, &n)) {
                            exe = p;
                            size_t s = exe.find_last_of(L"\\/");
                            if (s != std::wstring::npos) exe = exe.substr(s + 1);
                        }
                        ::CloseHandle(h);
                    }
                }
                lastResult = looksLikeMinecraft(title, exe);
                lastFg = fg; lastTitle = title;
            }
            res = lastResult;
        } else {
            lastFg = nullptr;
        }
        g_mcFocus.store(res);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

// ---- throwpot: troca para o slot da pocao, usa (RMB) e volta ao slot anterior ----
static void throwWorker() {
    int prev = 0;
    while (g_running.load()) {
        int vk = g_throwKey.load();
        bool d = (vk > 0) && keyDown(vk);
        if (d && !prev && !(g_hwnd && GetForegroundWindow() == g_hwnd)) {   // nunca dispara dentro do proprio app
            int from = g_curSlot.load();
            int slot = g_throwSlot.load();
            if (slot < 0) slot = 0; if (slot > 8) slot = 8;
            int delay = g_throwDelay.load();
            if (delay < 10) delay = 10; if (delay > 500) delay = 500;
            tapKey(g_hotbarVK[slot].load());
            g_curSlot.store(slot);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
            if (g_throwReturn.load() && from != slot) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                tapKey(g_hotbarVK[from].load());
                g_curSlot.store(from);
            }
        }
        prev = d ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---- W-tap: solta o W por alguns ms depois de um ataque (reseta o sprint) ------
static void wtapWorker() {
    auto last = std::chrono::steady_clock::now();
    bool prevPhysL = false;
    while (g_running.load()) {
        // sem "so durante o autoclick": um clique FISICO do mouse tambem dispara
        bool physL = g_physLeft.load();
        if (g_wtap.load() && !g_wtapOnlyAuto.load() && physL && !prevPhysL) g_wtapRequest.store(true);
        prevPhysL = physL;

        if (g_wtapRequest.exchange(false) && g_wtap.load() && keyDown('W') && windowOk()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 250) {
                int dl = g_wtapDelay.load();
                if (dl < 5) dl = 5; if (dl > 200) dl = 200;
                sendKey('W', true);
                std::this_thread::sleep_for(std::chrono::milliseconds(dl));
                sendKey('W', false);
                last = std::chrono::steady_clock::now();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// =============================================================================
//  Interface (ImGui): janela sem moldura, barra lateral, tema preto e vermelho
// =============================================================================
static const float kWinW = 760.0f, kWinH = 520.0f;   // area de cliente da janela
static const float kTitleH = 46.0f;                  // altura da barra de titulo propria
static const float kSideW  = 176.0f;                 // largura da barra lateral

#define WM_TRAYICON     (WM_APP + 1)                 // mensagens do icone da bandeja
#define WM_HIDE_TO_TRAY (WM_APP + 2)                 // pedido interno: esconder a janela na bandeja
#define ID_TRAY_SHOW    1001
#define ID_TRAY_EXIT    1002

static ImFont* g_fontBold = nullptr;                 // Segoe UI Bold (numero grande do CPS)
static int    s_page = 0;                            // 0 Clicker, 1 Macros, 2 Sounds, 3 Binds, 4 Themes, 5 Settings
static int    s_side = 0;                            // 0 = esquerdo, 1 = direito
static bool   g_pinned = false;                      // janela sempre no topo
static int    s_lastSound = 0;                       // para o interruptor "Click Sounds"
static std::string s_bindsMsg;                       // retorno do botao "Fetch binds"

static bool  s_capturing = false;                    // capturando uma tecla/botao?
static std::atomic<int>* s_captureKey = nullptr;     // onde gravar a tecla capturada
static bool  s_captureArmed = false;                 // ja soltou todas as teclas uma vez?
static std::chrono::steady_clock::time_point s_captureDeadline;   // a captura de tecla expira em 10 s
static const int kCaptureSeconds = 10;
static bool  s_countdownActive = false;
static std::chrono::steady_clock::time_point s_countdownEnd;

// Cor do tema (so a thread da UI mexe nisso). Vermelho por padrao.
static float g_accent[3]     = { 1.00f, 0.23f, 0.27f };
static float g_accentLive[3] = { 1.00f, 0.23f, 0.27f };   // cor realmente usada (muda no modo Rainbow)

// ---- salvar/carregar config (%APPDATA%\GNClicker\config.txt) ---------------
static std::string cfgPath() {
    const char* ap = getenv("APPDATA");
    std::string dir = ap ? (std::string(ap) + "\\GNClicker") : std::string("GNClicker");
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\config.txt";
}
static void saveConfig() {
    FILE* f = fopen(cfgPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "lang=%d\n", g_lang);
    fprintf(f, "clickMode=%d\n", g_clickMode.load());
    fprintf(f, "armKeyL=%d\n", g_left.armKey.load());
    fprintf(f, "armKeyR=%d\n", g_right.armKey.load());
    fprintf(f, "cpsL=%d\n",    g_left.cps.load());
    fprintf(f, "cpsR=%d\n",    g_right.cps.load());
    fprintf(f, "accR=%.4f\n",  g_accent[0]);
    fprintf(f, "accG=%.4f\n",  g_accent[1]);
    fprintf(f, "accB=%.4f\n",  g_accent[2]);
    fprintf(f, "soundName=%s\n", currentSoundName().c_str());
    fprintf(f, "inventory=%d\n", g_inventory.load() ? 1 : 0);
    fprintf(f, "blockhit=%d\n",  g_blockhit.load() ? 1 : 0);
    fprintf(f, "jitter=%d\n",    g_jitter.load() ? 1 : 0);
    fprintf(f, "jitterSpeed=%d\n", g_jitterSpeed.load());
    fprintf(f, "slotFilter=%d\n", g_slotFilter.load() ? 1 : 0);
    int mask = 0;
    for (int i = 0; i < 9; ++i) if (g_slotAllowed[i].load()) mask |= (1 << i);
    fprintf(f, "slotMask=%d\n", mask);
    for (int i = 0; i < 9; ++i) fprintf(f, "hotbar%d=%d\n", i, g_hotbarVK[i].load());
    fprintf(f, "throwKey=%d\n",   g_throwKey.load());
    fprintf(f, "throwSlot=%d\n",  g_throwSlot.load());
    fprintf(f, "throwDelay=%d\n", g_throwDelay.load());
    fprintf(f, "throwReturn=%d\n", g_throwReturn.load() ? 1 : 0);
    fprintf(f, "wtap=%d\n",       g_wtap.load() ? 1 : 0);
    fprintf(f, "wtapDelay=%d\n",  g_wtapDelay.load());
    fprintf(f, "wtapOnlyAuto=%d\n", g_wtapOnlyAuto.load() ? 1 : 0);
    fprintf(f, "rainbow=%d\n",    g_rainbow.load() ? 1 : 0);
    fclose(f);
}
static void loadConfig() {
    FILE* f = fopen(cfgPath().c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        while (!v.empty() && (v.back()=='\n' || v.back()=='\r' || v.back()==' ')) v.pop_back();
        if      (k == "lang")    g_lang = (atoi(v.c_str()) == 1) ? 1 : 0;
        else if (k == "clickMode") g_clickMode.store(atoi(v.c_str()) == 1 ? 1 : 0);
        else if (k == "armKeyL") g_left.armKey.store(atoi(v.c_str()));
        else if (k == "armKeyR") g_right.armKey.store(atoi(v.c_str()));
        else if (k == "cpsL")    { int iv=atoi(v.c_str()); g_left.cps.store(iv<1?1:(iv>100?100:iv)); }
        else if (k == "cpsR")    { int iv=atoi(v.c_str()); g_right.cps.store(iv<1?1:(iv>100?100:iv)); }
        else if (k == "accR")    g_accent[0] = (float)atof(v.c_str());
        else if (k == "accG")    g_accent[1] = (float)atof(v.c_str());
        else if (k == "accB")    g_accent[2] = (float)atof(v.c_str());
        else if (k == "soundName") setSoundByName(v);
        else if (k == "inventory")   g_inventory.store(atoi(v.c_str()) != 0);
        else if (k == "blockhit")    g_blockhit.store(atoi(v.c_str()) != 0);
        else if (k == "jitter")      g_jitter.store(atoi(v.c_str()) != 0);
        else if (k == "jitterSpeed") { int iv=atoi(v.c_str()); g_jitterSpeed.store(iv<1?1:(iv>15?15:iv)); }
        else if (k == "slotFilter")  g_slotFilter.store(atoi(v.c_str()) != 0);
        else if (k == "slotMask")    { int m=atoi(v.c_str()); for (int i=0;i<9;++i) g_slotAllowed[i].store((m>>i)&1); }
        else if (k.compare(0, 6, "hotbar") == 0 && k.size() == 7 && isdigit((unsigned char)k[6])) {
            int i = k[6]-'0'; int vk = atoi(v.c_str()); if (i < 9 && vk > 0) g_hotbarVK[i].store(vk);
        }
        else if (k == "throwKey")    g_throwKey.store(atoi(v.c_str()));
        else if (k == "throwSlot")   { int iv=atoi(v.c_str()); g_throwSlot.store(iv<0?0:(iv>8?8:iv)); }
        else if (k == "throwDelay")  { int iv=atoi(v.c_str()); g_throwDelay.store(iv<10?10:(iv>500?500:iv)); }
        else if (k == "throwReturn") g_throwReturn.store(atoi(v.c_str()) != 0);
        else if (k == "wtap")        g_wtap.store(atoi(v.c_str()) != 0);
        else if (k == "wtapDelay")   { int iv=atoi(v.c_str()); g_wtapDelay.store(iv<5?5:(iv>200?200:iv)); }
        else if (k == "wtapOnlyAuto") g_wtapOnlyAuto.store(atoi(v.c_str()) != 0);
        else if (k == "rainbow")     g_rainbow.store(atoi(v.c_str()) != 0);
    }
    fclose(f);
}

// Volta tudo ao padrao (botao "Reset settings").
static void resetDefaults() {
    initClickers();
    g_left.armed.store(false); g_right.armed.store(false);
    g_inventory.store(true); g_blockhit.store(false); g_jitter.store(false); g_jitterSpeed.store(2);
    g_slotFilter.store(false);
    g_throwKey.store(0); g_throwSlot.store(1); g_throwDelay.store(50); g_throwReturn.store(true);
    g_wtap.store(false); g_wtapDelay.store(30); g_wtapOnlyAuto.store(true);
    g_rainbow.store(false);
    g_clickMode.store(0);
    g_accent[0] = 1.00f; g_accent[1] = 0.23f; g_accent[2] = 0.27f;
    setSoundByName("Default");
    saveConfig();
}

// ---- cores e utilidades de desenho --------------------------------------------
static inline ImVec4 col(float r,float g,float b,float a){ return ImVec4(r,g,b,a); }
static inline ImVec4 mix(ImVec4 a, ImVec4 b, float t){
    return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t);
}
static inline ImVec4 alpha(ImVec4 c, float a){ return ImVec4(c.x,c.y,c.z,a); }
static inline ImU32  u32(ImVec4 c){ return ImGui::ColorConvertFloat4ToU32(c); }
static inline ImVec4 accentCol(){ return col(g_accentLive[0], g_accentLive[1], g_accentLive[2], 1.0f); }
static inline ImVec2 add2(ImVec2 a, ImVec2 b){ return ImVec2(a.x+b.x, a.y+b.y); }

static const ImVec4 kText  = ImVec4(0.95f, 0.95f, 0.97f, 1.0f);
static const ImVec4 kMuted = ImVec4(0.55f, 0.55f, 0.61f, 1.0f);

// Animacao suave 0..1 guardada por ID (para hover e interruptores).
static float animTo(ImGuiID id, float target, float speed = 14.0f) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, target);
    v += (target - v) * (1.0f - expf(-speed * ImGui::GetIO().DeltaTime));
    st->SetFloat(id, v);
    return v;
}

// Estilo base (chamado uma vez).
static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.ChildRounding = 12; s.FrameRounding = 9;
    s.PopupRounding = 9;  s.GrabRounding = 9;  s.ScrollbarRounding = 4; s.TabRounding = 8;
    s.WindowPadding = ImVec2(0,0); s.FramePadding = ImVec2(12,8);
    s.ItemSpacing = ImVec2(10,10); s.ItemInnerSpacing = ImVec2(8,6);
    s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.FrameBorderSize = 0; s.ScrollbarSize = 7;
    s.HoverDelayNormal = 3.0f;   // dicas das funcoes aparecem depois de ~3 s com o mouse parado em cima
}

// Dica (tooltip) com o resumo da funcao: aparece depois de ~3 s com o mouse em cima do ultimo item.
static void hoverTip(const char* tip) {
    if (!tip || !*tip) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Aplica o tema (chamado a cada frame -> acompanha a cor ao vivo e o modo Rainbow).
static void applyTheme() {
    ImVec4 a  = accentCol();
    ImVec4 hi = mix(a, col(1,1,1,1), 0.20f);
    ImVec4 lo = mix(a, col(0,0,0,1), 0.30f);
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_WindowBg]         = col(0.047f,0.047f,0.059f,1);
    c[ImGuiCol_ChildBg]          = col(0,0,0,0);
    c[ImGuiCol_PopupBg]          = col(0.07f,0.07f,0.09f,0.98f);
    c[ImGuiCol_Text]             = kText;
    c[ImGuiCol_TextDisabled]     = kMuted;
    c[ImGuiCol_Border]           = col(1,1,1,0.07f);
    c[ImGuiCol_FrameBg]          = col(0.10f,0.10f,0.125f,1);
    c[ImGuiCol_FrameBgHovered]   = col(0.14f,0.14f,0.17f,1);
    c[ImGuiCol_FrameBgActive]    = alpha(a, 0.35f);
    c[ImGuiCol_ScrollbarBg]      = col(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]    = col(1,1,1,0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = alpha(a, 0.55f);
    c[ImGuiCol_ScrollbarGrabActive]  = a;
    c[ImGuiCol_CheckMark]        = a;
    c[ImGuiCol_SliderGrab]       = a;
    c[ImGuiCol_SliderGrabActive] = hi;
    c[ImGuiCol_Button]           = alpha(a, 0.20f);
    c[ImGuiCol_ButtonHovered]    = alpha(a, 0.38f);
    c[ImGuiCol_ButtonActive]     = alpha(lo, 0.70f);
    c[ImGuiCol_Header]           = alpha(a, 0.22f);
    c[ImGuiCol_HeaderHovered]    = alpha(a, 0.35f);
    c[ImGuiCol_HeaderActive]     = alpha(a, 0.50f);
    c[ImGuiCol_Separator]        = col(1,1,1,0.07f);
    c[ImGuiCol_TextSelectedBg]   = alpha(a, 0.35f);
}

static void captureKeyStep() {
    if (!s_capturing || !s_captureKey) return;
    // a captura expira em 10 s sem tecla: cancela e mantem o bind que ja existia
    if (std::chrono::steady_clock::now() >= s_captureDeadline) {
        s_capturing = false;
        s_captureKey = nullptr;
        return;
    }
    // espera soltar tudo antes de aceitar a proxima tecla
    if (!s_captureArmed) {
        bool anyDown = false;
        for (int vk = 1; vk < 256; ++vk) if (GetAsyncKeyState(vk) & 0x8000) { anyDown = true; break; }
        if (!anyDown) s_captureArmed = true;
        return;
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
            s_captureKey->store(vk == VK_ESCAPE ? 0 : vk);   // Esc = deixar o bind vazio
            s_capturing = false;
            s_captureKey = nullptr;
            saveConfig();
            return;
        }
    }
}

// ---- widgets proprios -----------------------------------------------------------

// Linha com texto e interruptor liga/desliga (como nos apps modernos).
static bool toggleRow(const char* id, const char* label, bool* v, const char* hint = nullptr, const char* tip = nullptr) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 46.0f;
    ImGui::InvisibleButton("##row", ImVec2(w, h));
    hoverTip(tip);
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float t = animTo(ImGui::GetID("anim"), *v ? 1.0f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float ty = p.y + (h - ts.y) * 0.5f;
    dl->AddText(ImVec2(p.x, ty), u32(kText), label);
    if (hint) dl->AddText(ImVec2(p.x + ts.x + 10.0f, ty), u32(kMuted), hint);
    const float sw = 48.0f, sh = 26.0f;
    ImVec2 s0(p.x + w - sw, p.y + (h - sh) * 0.5f);
    dl->AddRectFilled(s0, ImVec2(s0.x + sw, s0.y + sh), u32(mix(col(0.17f,0.17f,0.20f,1), accentCol(), t)), sh * 0.5f);
    dl->AddCircleFilled(ImVec2(s0.x + sh * 0.5f + t * (sw - sh), s0.y + sh * 0.5f), sh * 0.5f - 3.5f, IM_COL32(255,255,255,255));
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), u32(col(1,1,1,0.06f)));
    ImGui::PopID();
    return changed;
}

// Slider fino com preenchimento na cor do tema e botao redondo.
static bool accentSlider(const char* id, const char* label, int* v, int vmin, int vmax, const char* tip = nullptr) {
    ImGui::PushID(id);
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(kMuted, "%s: %d", label, *v);
    hoverTip(tip);
    ImVec2 q = ImGui::GetCursorScreenPos();
    const float h = 26.0f;
    ImGui::InvisibleButton("##sl", ImVec2(w, h));
    hoverTip(tip);
    bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active) {
        float t = (ImGui::GetIO().MousePos.x - q.x - 10.0f) / (w - 20.0f);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        int nv = vmin + (int)(t * (vmax - vmin) + 0.5f);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = (float)(*v - vmin) / (float)(vmax - vmin);
    float y = q.y + h * 0.5f, x0 = q.x + 10.0f, x1 = q.x + w - 10.0f, gx = x0 + t * (x1 - x0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + 3), u32(col(0.17f,0.17f,0.20f,1)), 3.0f);
    dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(gx, y + 3), u32(acc), 3.0f);
    float r = active ? 11.0f : 10.0f;
    dl->AddCircleFilled(ImVec2(gx, y), r, u32(mix(acc, col(1,1,1,1), 0.55f)));
    dl->AddCircle(ImVec2(gx, y), r, u32(acc), 0, 2.0f);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PopID();
    return changed;
}

// Seletor de duas opcoes (ex.: Left Click | Right Click).
static bool segmented(const char* id, const char* a, const char* b, int* sel, float w = 280.0f, const char* tip = nullptr) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 38.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(col(0.09f,0.09f,0.11f,1)), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(col(1,1,1,0.07f)), h * 0.5f);
    bool changed = false;
    const char* names[2] = { a, b };
    for (int i = 0; i < 2; ++i) {
        ImVec2 c0(p.x + 4.0f + i * (w * 0.5f - 2.0f), p.y + 4.0f);
        ImVec2 c1(c0.x + w * 0.5f - 6.0f, p.y + h - 4.0f);
        ImGui::SetCursorScreenPos(c0);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(c1.x - c0.x, c1.y - c0.y)) && *sel != i) { *sel = i; changed = true; }
        hoverTip(tip);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        float t = animTo(ImGui::GetID("a"), (*sel == i) ? 1.0f : 0.0f);
        ImGui::PopID();
        if (t > 0.01f) dl->AddRectFilled(c0, c1, u32(alpha(accentCol(), 0.22f * t)), (c1.y - c0.y) * 0.5f);
        if (t > 0.01f) dl->AddRect(c0, c1, u32(alpha(accentCol(), 0.60f * t)), (c1.y - c0.y) * 0.5f);
        ImVec2 ts = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(c0.x + (c1.x - c0.x - ts.x) * 0.5f, c0.y + (c1.y - c0.y - ts.y) * 0.5f),
                    u32(mix(kMuted, accentCol(), t)), names[i]);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 4));
    ImGui::PopID();
    return changed;
}

// Botao de "bind": clique e depois pressione a tecla/botao desejado.
static void keyBindPill(const char* id, std::atomic<int>* target, float w = 150.0f) {
    ImGui::PushID(id);
    bool cap = s_capturing && s_captureKey == target;
    std::string txt;
    if (cap) {   // mostra a contagem regressiva de 10 s
        double left = std::chrono::duration<double>(s_captureDeadline - std::chrono::steady_clock::now()).count();
        txt = std::string(L("Press a key... ", "Pressione uma tecla... ")) + std::to_string((int)ceil(left < 0 ? 0 : left)) + "s";
    } else {
        txt = keyName(target->load());
    }
    const float h = 32.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##k", ImVec2(w, h)) && !cap) {
        s_capturing = true; s_captureKey = target; s_captureArmed = false;
        s_captureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(kCaptureSeconds);
    }
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(cap ? alpha(acc, 0.20f) : col(hov ? 0.15f : 0.11f, hov ? 0.15f : 0.11f, hov ? 0.18f : 0.14f, 1)), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(cap ? acc : col(1,1,1,0.10f)), h * 0.5f, 0, 1.4f);
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), u32(cap ? acc : kText), txt.c_str());
    ImGui::PopID();
}

// Linha "texto a esquerda, bind a direita".
static void bindRow(const char* id, const char* label, std::atomic<int>* target, const char* tip = nullptr) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 48.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), u32(kText), label);
    // area do nome da funcao (a esquerda do botao de bind): dica depois de ~3 s
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##lbl", ImVec2(w - 160.0f, h));
    hoverTip(tip);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 150.0f, p.y + (h - 32.0f) * 0.5f));
    keyBindPill("pill", target, 150.0f);
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), u32(col(1,1,1,0.06f)));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 2.0f));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
}

// Cabecalho de secao (texto pequeno em maiusculas).
static void sectionTitle(const char* text, const char* tip = nullptr) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(alpha(accentCol(), 0.95f), "%s", text);
    hoverTip(tip);
    ImGui::Dummy(ImVec2(0, 0));
}

// Botao simples no estilo do tema.
static bool pillButton(const char* label, float w = 0.0f, const char* tip = nullptr) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18, 8));
    bool r = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleVar(2);
    hoverTip(tip);
    return r;
}

// ---- icones da barra lateral (desenhados com formas simples) ----------------------
static void drawIcon(int id, ImVec2 c, ImU32 colr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    switch (id) {
    case 0: { // raio (Clicker)
        ImVec2 pts[6] = { {c.x+2,c.y-8}, {c.x-5,c.y+1}, {c.x-0.5f,c.y+1}, {c.x-2,c.y+8}, {c.x+5,c.y-1}, {c.x+0.5f,c.y-1} };
        dl->AddConcavePolyFilled(pts, 6, colr); break; }
    case 1: { // controles deslizantes (Macros)
        for (int i = 0; i < 3; ++i) {
            float y = c.y - 6.0f + i * 6.0f, kx = c.x + ((i == 1) ? 3.0f : -3.0f);
            dl->AddLine(ImVec2(c.x - 8, y), ImVec2(c.x + 8, y), colr, 1.6f);
            dl->AddCircleFilled(ImVec2(kx, y), 2.6f, colr);
        } break; }
    case 2: { // alto-falante (Sounds)
        dl->AddRectFilled(ImVec2(c.x-8,c.y-3), ImVec2(c.x-4,c.y+3), colr, 1.0f);
        ImVec2 pts[4] = { {c.x-4,c.y-3}, {c.x+1,c.y-7}, {c.x+1,c.y+7}, {c.x-4,c.y+3} };
        dl->AddConvexPolyFilled(pts, 4, colr);
        dl->PathArcTo(ImVec2(c.x+1, c.y), 5.0f, -0.9f, 0.9f); dl->PathStroke(colr, 0, 1.5f);
        dl->PathArcTo(ImVec2(c.x+1, c.y), 8.0f, -0.9f, 0.9f); dl->PathStroke(colr, 0, 1.5f); break; }
    case 3: { // teclado (Binds)
        dl->AddRect(ImVec2(c.x-9,c.y-5.5f), ImVec2(c.x+9,c.y+5.5f), colr, 2.5f, 0, 1.5f);
        for (int i = 0; i < 4; ++i) dl->AddCircleFilled(ImVec2(c.x-5.5f+i*3.7f, c.y-1.8f), 0.9f, colr);
        dl->AddLine(ImVec2(c.x-4,c.y+2.4f), ImVec2(c.x+4,c.y+2.4f), colr, 1.5f); break; }
    case 4: { // paleta (Themes)
        dl->AddCircle(c, 8.5f, colr, 0, 1.6f);
        dl->AddCircleFilled(ImVec2(c.x-3.5f,c.y-2.5f), 1.5f, colr);
        dl->AddCircleFilled(ImVec2(c.x+1.0f,c.y-4.0f), 1.5f, colr);
        dl->AddCircleFilled(ImVec2(c.x+4.0f,c.y+0.5f), 1.5f, colr); break; }
    case 5: { // engrenagem (Settings)
        dl->AddCircle(c, 3.2f, colr, 0, 1.6f);
        dl->AddCircle(c, 6.4f, colr, 0, 1.6f);
        for (int i = 0; i < 8; ++i) {
            float a = i * 0.7853982f;
            dl->AddLine(ImVec2(c.x + cosf(a)*6.4f, c.y + sinf(a)*6.4f), ImVec2(c.x + cosf(a)*9.0f, c.y + sinf(a)*9.0f), colr, 2.0f);
        } break; }
    }
}

static void sidebarItem(int idx, const char* label) {
    ImGui::PushID(idx);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = kSideW - 24.0f, h = 40.0f;
    if (ImGui::InvisibleButton("##nav", ImVec2(w, h))) s_page = idx;
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float t = animTo(ImGui::GetID("a"), (s_page == idx) ? 1.0f : 0.0f);
    float hv = animTo(ImGui::GetID("h"), hov ? 1.0f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    float bgA = 0.17f * t + 0.07f * hv * (1.0f - t);
    if (bgA > 0.002f) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(alpha(acc, bgA)), 11.0f);
    if (t > 0.01f) dl->AddRectFilled(ImVec2(p.x, p.y + 9), ImVec2(p.x + 3, p.y + h - 9), u32(alpha(acc, t)), 2.0f);
    ImVec4 tc = mix(col(0.70f,0.70f,0.76f,1), acc, t);
    drawIcon(idx, ImVec2(p.x + 24.0f, p.y + h * 0.5f), u32(tc));
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 46.0f, p.y + (h - ts.y) * 0.5f), u32(mix(col(0.78f,0.78f,0.83f,1), acc, t)), label);
    ImGui::PopID();
}

// ---- paginas -------------------------------------------------------------------------

static void pageClicker() {
    Clicker* c = (s_side == 0) ? &g_left : &g_right;
    bool leftSide = (s_side == 0);
    segmented("side", L("Left Click", "Clique Esquerdo"), L("Right Click", "Clique Direito"), &s_side, 300.0f,
              L("Choose which mouse button you are setting up. Each side has its own CPS, hotkey and on/off state.",
                "Escolha qual botão do mouse você está configurando. Cada lado tem seu próprio CPS, tecla e estado ligado/desligado."));
    ImGui::Dummy(ImVec2(0, 2));

    // cartao grande com o CPS
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 124.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(mix(col(0.075f,0.055f,0.065f,1), acc, 0.09f)), 18.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(alpha(acc, 0.45f)), 18.0f, 0, 1.5f);
    int cps = c->cps.load();
    char num[8]; snprintf(num, sizeof(num), "%d", cps);
    ImGui::PushFont(g_fontBold, 72.0f);
    ImVec2 ns = ImGui::CalcTextSize(num);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ns.x) * 0.5f, p.y + 6.0f), u32(acc), num);
    ImGui::PopFont();
    const char* lab = "C P S";
    ImVec2 ls = ImGui::CalcTextSize(lab);
    dl->AddText(ImVec2(p.x + (w - ls.x) * 0.5f, p.y + h - 28.0f), u32(kMuted), lab);
    // area central do cartao: so para a dica
    ImGui::SetCursorScreenPos(ImVec2(p.x + 90.0f, p.y));
    ImGui::InvisibleButton("##cpsTip", ImVec2(w - 180.0f, h));
    hoverTip(L("CPS = clicks per second. A higher number clicks faster. Many games and servers limit how fast clicks count.",
               "CPS = cliques por segundo. Quanto maior o número, mais rápido ele clica. Muitos jogos e servidores limitam a velocidade dos cliques."));
    // botoes - e + nas laterais do cartao
    for (int i = 0; i < 2; ++i) {
        ImVec2 bc(i == 0 ? p.x + 44.0f : p.x + w - 44.0f, p.y + h * 0.5f - 6.0f);
        ImGui::SetCursorScreenPos(ImVec2(bc.x - 20.0f, bc.y - 20.0f));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##step", ImVec2(40, 40))) {
            int nv = cps + (i == 0 ? -1 : 1); nv = nv < 1 ? 1 : (nv > 100 ? 100 : nv);
            c->cps.store(nv); saveConfig();
        }
        bool hv = ImGui::IsItemHovered();
        if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddCircleFilled(bc, 18.0f, u32(alpha(acc, hv ? 0.28f : 0.14f)));
        dl->AddLine(ImVec2(bc.x - 6, bc.y), ImVec2(bc.x + 6, bc.y), u32(acc), 2.2f);
        if (i == 1) dl->AddLine(ImVec2(bc.x, bc.y - 6), ImVec2(bc.x, bc.y + 6), u32(acc), 2.2f);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 10.0f));
    ImGui::Dummy(ImVec2(w, 0));

    int sv = cps;
    if (accentSlider("cps", L("Clicks per second", "Cliques por segundo"), &sv, 1, 100,
                     L("Drag to set how many times per second this side clicks (1 to 100).",
                       "Arraste para definir quantas vezes por segundo este lado clica (1 a 100)."))) { c->cps.store(sv); saveConfig(); }

    bool armed = c->armed.load();
    char hintA[64]; snprintf(hintA, sizeof(hintA), "[%s]", keyName(c->armKey.load()).c_str());
    if (toggleRow("auto", L("Auto Click", "Clique Automático"), &armed, hintA,
                  L("Turns the auto clicker on or off for this side (its hotkey does the same). Works in any program, not only Minecraft. By default you also hold the mouse button to click; Settings has an option to click without holding.",
                    "Liga ou desliga o clique automático deste lado (a tecla dele faz o mesmo). Funciona em qualquer programa, não só no Minecraft. Por padrão você também segura o botão do mouse para clicar; em Settings há uma opção para clicar sem segurar."))) c->armed.store(armed);

    // Jitter e Blockhit so existem para o clique ESQUERDO
    if (leftSide) {
        bool jt = g_jitter.load();
        if (toggleRow("jitter", "Jitter", &jt, L("(Minecraft only)", "(só no Minecraft)"),
                      L("Adds tiny random mouse movements while auto clicking, so your aim looks less robotic. It only works while Minecraft (or a known Minecraft launcher or client) is the active window; the auto click itself works everywhere.",
                        "Adiciona pequenos movimentos aleatórios do mouse enquanto clica, para a mira parecer menos robótica. Só funciona quando o Minecraft (ou um launcher/client conhecido) é a janela ativa; o clique automático em si funciona em qualquer lugar."))) { g_jitter.store(jt); saveConfig(); }
        if (jt) {
            int sp = g_jitterSpeed.load();
            if (accentSlider("jspeed", L("Jitter intensity", "Intensidade do jitter"), &sp, 1, 15,
                             L("How far the mouse moves on each click, in pixels (1 to 15). Lower is subtler.",
                               "Quanto o mouse se move a cada clique, em pixels (1 a 15). Menor é mais discreto."))) { g_jitterSpeed.store(sp); saveConfig(); }
        }
        bool bh = g_blockhit.load();
        if (toggleRow("bh", "Blockhit", &bh, L("(hold right click)", "(segure o botão direito)"),
                      L("Minecraft PvP: lets you keep blocking with a sword while you hit. While you hold the right mouse button, each automatic left click briefly releases the block and then blocks again. Left click only.",
                        "PvP do Minecraft: deixa você continuar bloqueando com a espada enquanto bate. Enquanto você segura o botão direito do mouse, cada clique esquerdo automático solta o bloqueio por um instante e bloqueia de novo. Só no clique esquerdo."))) { g_blockhit.store(bh); saveConfig(); }
    }
    bool inv = g_inventory.load();
    if (toggleRow("inv", "Inventory", &inv, L("(click with the cursor visible)", "(clicar com o cursor visível)"),
                  L("Allows clicking while the mouse cursor is visible (menus, inventories, the desktop). When off, it only clicks while the cursor is hidden, which is what happens inside a game.",
                    "Permite clicar enquanto o cursor do mouse está visível (menus, inventários, área de trabalho). Desligado, só clica com o cursor escondido, que é o que acontece dentro de um jogo."))) { g_inventory.store(inv); saveConfig(); }
    bool snd = g_sound.load() >= 0;
    if (toggleRow("snd", L("Click Sounds", "Sons de Clique"), &snd, nullptr,
                  L("Plays a sound on every automatic click. Pick which sound on the Sounds page.",
                    "Toca um som a cada clique automático. Escolha o som na página Sounds."))) {
        if (snd) g_sound.store(s_lastSound >= 0 ? s_lastSound : 0); else { s_lastSound = g_sound.load(); g_sound.store(-1); }
        saveConfig();
    }
    bool rs = g_restrict.load();
    if (toggleRow("win", L("Target window only", "Só na janela alvo"), &rs, nullptr,
                  L("Limits the auto clicker to a single window. Press Capture, switch to that window within 3 seconds, and it will only click while that window is active.",
                    "Limita o clique automático a uma única janela. Aperte Capturar, troque para essa janela em até 3 segundos, e ele só clica enquanto ela estiver ativa."))) g_restrict.store(rs);
    // captura de janela alvo
    if (s_countdownActive) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(s_countdownEnd - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            HWND fg = ::GetForegroundWindow();
            g_targetHwnd.store((uintptr_t)fg);
            char t[128] = {0};
            ::GetWindowTextA(fg, t, 127);
            g_targetTitle = t[0] ? t : L("(no title)", "(sem título)");
            g_restrict.store(true);
            s_countdownActive = false;
        } else {
            std::string lb = std::string(L("Focus your game... ", "Foque o jogo... ")) + std::to_string(left / 1000 + 1);
            pillButton(lb.c_str());
        }
    } else if (pillButton(L("Capture window (3s)", "Capturar janela (3s)"), 0.0f,
                          L("Starts a 3 second countdown. Switch to the window you want to limit the auto clicker to.",
                            "Inicia uma contagem de 3 segundos. Troque para a janela à qual você quer limitar o clique automático."))) {
        s_countdownActive = true;
        s_countdownEnd = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s", g_targetTitle.empty() ? L("(none)", "(nenhuma)") : g_targetTitle.c_str());
    ImGui::Dummy(ImVec2(0, 8));
}

static void pageMacros() {
    sectionTitle(L("SLOT WHITELIST", "WHITELIST DE SLOTS"),
                 L("Minecraft hotbar filter: the auto clicker only works while one of the ticked hotbar slots (1 to 9) is selected. Handy to click only with a sword, and not with a block or a bow.",
                   "Filtro da hotbar do Minecraft: o clique automático só funciona enquanto um dos slots marcados (1 a 9) está selecionado. Útil para clicar só com a espada, e não com um bloco ou arco."));
    bool sf = g_slotFilter.load();
    if (toggleRow("sf", L("Click only on selected slots", "Clicar só nos slots marcados"), &sf, nullptr,
                  L("When on, the auto clicker only clicks while the selected hotbar slot is one of the ticked numbers below.",
                    "Ligado, o clique automático só clica quando o slot selecionado da hotbar é um dos números marcados abaixo."))) { g_slotFilter.store(sf); saveConfig(); }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    int cur = g_curSlot.load();
    for (int i = 0; i < 9; ++i) {
        ImVec2 p(p0.x + i * 54.0f, p0.y + 2.0f);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool on = g_slotAllowed[i].load();
        if (ImGui::InvisibleButton("##slot", ImVec2(46, 46))) { g_slotAllowed[i].store(!on); on = !on; saveConfig(); }
        hoverTip(L("Click a number to allow or block that hotbar slot. The small dot shows the slot you are probably holding.",
                   "Clique em um número para permitir ou bloquear esse slot da hotbar. O pontinho mostra o slot que você provavelmente está segurando."));
        bool hv = ImGui::IsItemHovered();
        if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(p, ImVec2(p.x + 46, p.y + 46), u32(on ? alpha(acc, hv ? 0.30f : 0.20f) : col(hv ? 0.14f : 0.10f, hv ? 0.14f : 0.10f, hv ? 0.17f : 0.125f, 1)), 12.0f);
        dl->AddRect(p, ImVec2(p.x + 46, p.y + 46), u32(on ? acc : col(1,1,1,0.08f)), 12.0f, 0, 1.4f);
        char n[4]; snprintf(n, sizeof(n), "%d", i + 1);
        ImVec2 ts = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(p.x + (46 - ts.x) * 0.5f, p.y + (46 - ts.y) * 0.5f), u32(on ? acc : kMuted), n);
        if (i == cur) dl->AddCircleFilled(ImVec2(p.x + 23, p.y + 40), 2.2f, u32(acc));
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 56.0f));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::TextColored(kMuted, "%s %d", L("Current slot (estimated):", "Slot atual (estimado):"), cur + 1);
    if (pillButton(L("Fetch Minecraft binds", "Ler binds do Minecraft"), 0.0f,
                   L("Reads your hotbar keys from Minecraft's options.txt, so slot detection matches your own key binds. Read-only: nothing is changed in the game.",
                     "Lê as teclas da sua hotbar no options.txt do Minecraft, para a detecção de slot bater com as suas teclas. Somente leitura: nada é alterado no jogo."))) {
        s_bindsMsg = fetchMinecraftBinds() ? L("Binds loaded from Minecraft.", "Binds lidos do Minecraft.")
                                           : L("options.txt not found (or default binds).", "options.txt não encontrado (ou binds padrão).");
        saveConfig();
    }
    ImGui::SameLine(); ImGui::TextColored(kMuted, "%s", s_bindsMsg.c_str());

    sectionTitle("THROWPOT",
                 L("Minecraft potion macro: one key press switches to your potion slot, throws the potion (right click) and, if enabled, goes back to the slot you were holding.",
                   "Macro de poção do Minecraft: um aperto de tecla troca para o slot da poção, arremessa (clique direito) e, se ativado, volta ao slot que você estava segurando."));
    bindRow("tpk", L("Key", "Tecla"), &g_throwKey,
            L("The key that runs the Throwpot macro. Empty means the macro is off.",
              "A tecla que executa a macro Throwpot. Vazio significa macro desligada."));
    int ts = g_throwSlot.load() + 1;
    if (accentSlider("tps", L("Potion slot", "Slot da poção"), &ts, 1, 9,
                     L("The hotbar slot (1 to 9) where you keep your potions.",
                       "O slot da hotbar (1 a 9) onde ficam as suas poções."))) { g_throwSlot.store(ts - 1); saveConfig(); }
    int td = g_throwDelay.load();
    if (accentSlider("tpd", L("Delay (ms)", "Delay (ms)"), &td, 10, 500,
                     L("Pause between each step of the macro, in milliseconds. Raise it if the game sometimes misses the slot switch.",
                       "Pausa entre cada passo da macro, em milissegundos. Aumente se o jogo às vezes perder a troca de slot."))) { g_throwDelay.store(td); saveConfig(); }
    bool tr = g_throwReturn.load();
    if (toggleRow("tpr", L("Return to previous slot", "Voltar ao slot anterior"), &tr, nullptr,
                  L("After throwing, switch back to the slot you were holding before.",
                    "Depois de arremessar, volta para o slot que você estava segurando antes."))) { g_throwReturn.store(tr); saveConfig(); }

    sectionTitle("W-TAP",
                 L("Minecraft sprint reset: right after a hit, the W key (forward) is released for a few milliseconds and pressed again. This resets your sprint, which can increase knockback.",
                   "Reset de sprint do Minecraft: logo depois de um golpe, a tecla W (frente) é solta por alguns milissegundos e apertada de novo. Isso reseta o sprint, o que pode aumentar o knockback."));
    bool wt = g_wtap.load();
    if (toggleRow("wt", L("Enable W-Tap", "Ativar W-Tap"), &wt, nullptr,
                  L("Turns the W-Tap macro on or off.", "Liga ou desliga a macro W-Tap."))) { g_wtap.store(wt); saveConfig(); }
    int wd = g_wtapDelay.load();
    if (accentSlider("wtd", L("Delay (ms)", "Delay (ms)"), &wd, 5, 200,
                     L("How long W stays released, in milliseconds.", "Por quanto tempo o W fica solto, em milissegundos."))) { g_wtapDelay.store(wd); saveConfig(); }
    bool wo = g_wtapOnlyAuto.load();
    if (toggleRow("wto", L("Only while autoclicking", "Só durante o autoclick"), &wo, nullptr,
                  L("On: W-Tap only reacts to the auto clicker's hits. Off: it also reacts to your own manual left clicks.",
                    "Ligado: o W-Tap só reage aos golpes do clique automático. Desligado: também reage aos seus cliques esquerdos manuais."))) { g_wtapOnlyAuto.store(wo); saveConfig(); }
    ImGui::Dummy(ImVec2(0, 8));
}

static void pageSounds() {
    ImGui::TextColored(kMuted, "%s", L("Pick the click sound. Click a row to preview it.", "Escolha o som do clique. Clique em uma linha para ouvir."));
    ImGui::Dummy(ImVec2(0, 2));
    int cur = g_sound.load();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    int total = (int)g_sndList.size() + 1;        // +1 = "Nenhum"
    for (int row = 0; row < total; ++row) {
        int idx = row - 1;                         // -1 = nenhum
        const char* name = (row == 0) ? L("None", "Nenhum") : g_sndList[idx].name.c_str();
        ImGui::PushID(row);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 42.0f;
        if (ImGui::InvisibleButton("##snd", ImVec2(w, h))) {
            g_sound.store(idx); if (idx >= 0) s_lastSound = idx; saveConfig(); playClickSound();
        }
        bool hv = ImGui::IsItemHovered(), sel = (cur == idx);
        if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        float t = animTo(ImGui::GetID("a"), sel ? 1.0f : 0.0f);
        if (t > 0.01f || hv) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h - 4), u32(alpha(acc, 0.16f * t + (hv ? 0.06f : 0.0f))), 11.0f);
        ImVec2 c(p.x + 22, p.y + (h - 4) * 0.5f);
        dl->AddCircle(c, 8.0f, u32(sel ? acc : col(1,1,1,0.25f)), 0, 1.6f);
        if (t > 0.01f) dl->AddCircleFilled(c, 4.2f * t, u32(acc));
        ImVec2 ts = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(p.x + 44, p.y + (h - 4 - ts.y) * 0.5f), u32(mix(kText, acc, t * 0.6f)), name);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(kMuted, "%s", L("Tip: drop .wav files (up to 5 MB) in the \"sounds\" folder next to the program.",
                                       "Dica: coloque arquivos .wav (até 5 MB) na pasta \"sounds\" ao lado do programa."));
    ImGui::Dummy(ImVec2(0, 8));
}

static void pageBinds() {
    ImGui::TextColored(kMuted, "%s", L("Click a button, then press the key (or mouse button) you want.",
                                       "Clique em um botão e aperte a tecla (ou botão do mouse) que quiser."));
    ImGui::TextColored(kMuted, "%s", L("Esc clears the bind. With no key pressed for 10 seconds, it is cancelled.",
                                       "Esc deixa o bind vazio. Sem apertar nenhuma tecla por 10 segundos, ele é cancelado."));
    ImGui::Dummy(ImVec2(0, 4));
    bindRow("bl", L("Left clicker (arm / disarm)", "Clicker esquerdo (armar / desarmar)"), &g_left.armKey,
            L("The key that turns the left auto clicker on and off. It works even while a game is in focus.",
              "A tecla que liga e desliga o clique automático esquerdo. Funciona mesmo com um jogo em foco."));
    bindRow("br", L("Right clicker (arm / disarm)", "Clicker direito (armar / desarmar)"), &g_right.armKey,
            L("The key that turns the right auto clicker on and off.",
              "A tecla que liga e desliga o clique automático direito."));
    bindRow("bt", "Throwpot", &g_throwKey,
            L("The key that runs the Throwpot macro (switch to the potion slot, throw, return). Empty means off.",
              "A tecla que executa a macro Throwpot (troca para o slot da poção, arremessa, volta). Vazio significa desligada."));
    ImGui::Dummy(ImVec2(0, 6));
    if (pillButton(L("Fetch Minecraft binds", "Ler binds do Minecraft"), 0.0f,
                   L("Reads your hotbar keys from Minecraft's options.txt, so slot detection matches your own key binds. Read-only: nothing is changed in the game.",
                     "Lê as teclas da sua hotbar no options.txt do Minecraft, para a detecção de slot bater com as suas teclas. Somente leitura: nada é alterado no jogo."))) {
        s_bindsMsg = fetchMinecraftBinds() ? L("Binds loaded from Minecraft.", "Binds lidos do Minecraft.")
                                           : L("options.txt not found (or default binds).", "options.txt não encontrado (ou binds padrão).");
        saveConfig();
    }
    ImGui::SameLine(); ImGui::TextColored(kMuted, "%s", s_bindsMsg.c_str());
    ImGui::Dummy(ImVec2(0, 8));
}

static void pageThemes() {
    ImGui::TextColored(kMuted, "%s", L("Choose the accent color.", "Escolha a cor de destaque."));
    ImGui::Dummy(ImVec2(0, 4));
    struct Preset { float r, g, b; };
    static const Preset presets[] = {
        {1.00f,0.23f,0.27f}, {0.86f,0.08f,0.24f}, {1.00f,0.50f,0.15f}, {1.00f,0.42f,0.72f},
        {0.62f,0.45f,1.00f}, {0.35f,0.65f,1.00f}, {0.28f,0.85f,0.50f}, {0.92f,0.92f,0.95f},
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 8; ++i) {
        ImVec2 c(p0.x + 22 + i * 52.0f, p0.y + 22);
        ImGui::SetCursorScreenPos(ImVec2(c.x - 22, c.y - 22));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##sw", ImVec2(44, 44))) { g_accent[0]=presets[i].r; g_accent[1]=presets[i].g; g_accent[2]=presets[i].b; saveConfig(); }
        bool hv = ImGui::IsItemHovered();
        if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImU32 fc = u32(col(presets[i].r, presets[i].g, presets[i].b, 1));
        bool sel = fabsf(g_accent[0]-presets[i].r) < 0.01f && fabsf(g_accent[1]-presets[i].g) < 0.01f && fabsf(g_accent[2]-presets[i].b) < 0.01f;
        dl->AddCircleFilled(c, hv ? 19.0f : 17.0f, fc);
        if (sel) dl->AddCircle(c, 22.0f, u32(col(1,1,1,0.9f)), 0, 2.0f);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 56.0f));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::TextColored(kMuted, "%s", L("Custom color", "Cor personalizada"));
    if (ImGui::ColorEdit3("##custom", g_accent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueBar)) saveConfig();
    ImGui::Dummy(ImVec2(0, 4));
    bool rb = g_rainbow.load();
    if (toggleRow("rb", "Rainbow", &rb, nullptr,
                  L("Slowly cycles the accent color through every color of the rainbow.",
                    "Faz a cor de destaque percorrer lentamente todas as cores do arco-íris."))) { g_rainbow.store(rb); saveConfig(); }
    ImGui::Dummy(ImVec2(0, 8));
}

static void pageSettings() {
    ImGui::TextColored(kMuted, "%s", L("Language", "Idioma"));
    int lg = g_lang;
    if (segmented("lang", "English", "Português", &lg, 300.0f,
                  L("Changes the language of the whole interface.", "Muda o idioma de toda a interface."))) { g_lang = lg; saveConfig(); }
    ImGui::Dummy(ImVec2(0, 4));

    // modo de ativacao do clique (fica aqui, escondido nas configuracoes)
    sectionTitle(L("ACTIVATION MODE", "MODO DE ATIVAÇÃO"),
                 L("How the auto clicker starts clicking after you arm it with its hotkey.",
                   "Como o clique automático começa a clicar depois que você o arma com a tecla dele."));
    int mode = g_clickMode.load();
    if (segmented("mode", L("Hold the button", "Segurar o botão"), L("Press once", "Apertar uma vez"), &mode, 360.0f,
                  L("Hold the button (default): it clicks only while you hold the mouse button. Press once: pressing the hotkey starts continuous clicking and pressing it again stops it, like a classic autoclicker.",
                    "Segurar o botão (padrão): ele só clica enquanto você segura o botão do mouse. Apertar uma vez: apertar a tecla começa o clique contínuo e apertar de novo para, como um autoclicker comum."))) { g_clickMode.store(mode); saveConfig(); }
    ImGui::TextColored(kMuted, "%s", mode == 1
        ? L("Press the hotkey once to click non-stop; press it again to stop.",
            "Aperte a tecla uma vez para clicar sem parar; aperte de novo para parar.")
        : L("Arm with the hotkey, then hold the mouse button to click.",
            "Arme com a tecla e segure o botão do mouse para clicar."));
    ImGui::Dummy(ImVec2(0, 6));

    bool pin = g_pinned;
    if (toggleRow("pin", L("Always on top", "Sempre no topo"), &pin, nullptr,
                  L("Keeps the GNClicker window above all other windows.",
                    "Mantém a janela do GNClicker acima de todas as outras."))) {
        g_pinned = pin;
        ::SetWindowPos(g_hwnd, pin ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (pillButton(L("Reset settings", "Restaurar padrões"), 0.0f,
                   L("Restores every setting to its default value.",
                     "Restaura todas as configurações para os valores padrão."))) resetDefaults();

    ImGui::Dummy(ImVec2(0, 14));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 108.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(mix(col(0.075f,0.055f,0.065f,1), acc, 0.06f)), 16.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(alpha(acc, 0.30f)), 16.0f);
    if (g_logoSRV) dl->AddImage((ImTextureID)g_logoSRV, ImVec2(p.x + 16, p.y + 16), ImVec2(p.x + 76, p.y + 76));
    dl->AddText(ImVec2(p.x + 92, p.y + 16), u32(kText), "GNClicker 1.0.3");
    dl->AddText(ImVec2(p.x + 92, p.y + 40), u32(kMuted), L("Made in Brazil by Gazake", "Feito no Brasil por Gazake"));
    dl->AddText(ImVec2(p.x + 92, p.y + 62), u32(kMuted), L("MIT License. Interface: Dear ImGui (MIT).", "Licença MIT. Interface: Dear ImGui (MIT)."));
    dl->AddText(ImVec2(p.x + 92, p.y + 82), u32(alpha(kMuted, 0.8f)), L("No network, no persistence, no trace wiping.", "Sem rede, sem persistência, sem apagar rastros."));
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, 8));
}

// ---- janela principal -------------------------------------------------------------------
static void drawUI() {
    captureKeyStep();
    // cor do tema: a escolhida, ou o arco-iris (so visual, nao altera a cor salva)
    if (g_rainbow.load()) {
        float h = (float)fmod(ImGui::GetTime() * 0.12, 1.0);
        ImGui::ColorConvertHSVtoRGB(h, 0.65f, 1.0f, g_accentLive[0], g_accentLive[1], g_accentLive[2]);
    } else {
        g_accentLive[0] = g_accent[0]; g_accentLive[1] = g_accent[1]; g_accentLive[2] = g_accent[2];
    }
    applyTheme();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##gn", nullptr, flags);
    ImVec2 o = ImGui::GetWindowPos();
    float W = vp->WorkSize.x, H = vp->WorkSize.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 acc = accentCol();

    // fundos: barra de titulo, barra lateral
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + kTitleH), u32(col(0.067f,0.067f,0.083f,1)));
    dl->AddLine(ImVec2(o.x, o.y + kTitleH), ImVec2(o.x + W, o.y + kTitleH), u32(col(1,1,1,0.07f)));
    dl->AddRectFilled(ImVec2(o.x, o.y + kTitleH + 1), ImVec2(o.x + kSideW, o.y + H), u32(col(0.035f,0.035f,0.045f,1)));
    dl->AddLine(ImVec2(o.x + kSideW, o.y + kTitleH + 1), ImVec2(o.x + kSideW, o.y + H), u32(col(1,1,1,0.07f)));
    dl->AddRect(o, ImVec2(o.x + W, o.y + H), u32(alpha(acc, 0.22f)), 0.0f, 0, 1.0f);

    // barra de titulo: tres bolinhas (fechar, minimizar, fixar no topo) + logo + nome
    struct Dot { float x; ImU32 c; const char* tip; };
    Dot dots[3] = { { 24, IM_COL32(255,95,87,255),  L("Close", "Fechar") },
                    { 46, IM_COL32(254,188,46,255), L("Minimize", "Minimizar") },
                    { 68, IM_COL32(40,200,64,255),  L("Hide to tray (keeps clicking in the background)", "Esconder na bandeja (continua funcionando)") } };
    for (int i = 0; i < 3; ++i) {
        ImVec2 c(o.x + dots[i].x, o.y + kTitleH * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - 9, c.y - 9));
        ImGui::PushID(100 + i);
        if (ImGui::InvisibleButton("##dot", ImVec2(18, 18))) {
            if (i == 0) ::PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            if (i == 1) ::ShowWindow(g_hwnd, SW_MINIMIZE);
            if (i == 2) ::PostMessageW(g_hwnd, WM_HIDE_TO_TRAY, 0, 0);   // bolinha verde: esconde na bandeja do sistema
        }
        bool hv = ImGui::IsItemHovered();
        if (hv) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("%s", dots[i].tip); }
        dl->AddCircleFilled(c, hv ? 7.5f : 6.5f, dots[i].c);
        ImGui::PopID();
    }
    if (g_logoSRV) dl->AddImage((ImTextureID)g_logoSRV, ImVec2(o.x + 92, o.y + 9), ImVec2(o.x + 92 + 28, o.y + 9 + 28));
    ImGui::PushFont(g_fontBold, 15.0f);
    dl->AddText(ImVec2(o.x + 128, o.y + (kTitleH - ImGui::GetFontSize()) * 0.5f), u32(col(0.78f,0.78f,0.84f,1)), "GNClicker");
    ImGui::PopFont();

    // barra lateral
    ImGui::SetCursorScreenPos(ImVec2(o.x + 12, o.y + kTitleH + 16));
    const char* names[6] = { "Clicker", "Macros", "Sounds", "Binds", "Themes", "Settings" };
    const char* namesPt[6] = { "Clicker", "Macros", "Sons", "Teclas", "Temas", "Config." };
    for (int i = 0; i < 6; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(o.x + 12, o.y + kTitleH + 16 + i * 46.0f));
        sidebarItem(i, g_lang == 1 ? namesPt[i] : names[i]);
    }

    // pagina (area rolavel)
    ImGui::SetCursorScreenPos(ImVec2(o.x + kSideW + 1, o.y + kTitleH + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 20));
    ImGui::BeginChild("##page", ImVec2(W - kSideW - 1, H - kTitleH - 1), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    switch (s_page) {
        case 0: pageClicker();  break;
        case 1: pageMacros();   break;
        case 2: pageSounds();   break;
        case 3: pageBinds();    break;
        case 4: pageThemes();   break;
        default: pageSettings(); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

// =============================================================================
//  main (scaffolding DX11/Win32 do exemplo oficial do Dear ImGui - MIT)
// =============================================================================
int main(int, char**) {
    timeBeginPeriod(1);   // timer de 1 ms: ritmo de clique e som regulares (o padrao e ~15 ms)
    {   // Windows 10/11: pede para NAO limitar o timer/CPU deste processo quando a janela esta em segundo plano
        struct PowerThrottling { ULONG Version; ULONG ControlMask; ULONG StateMask; };
        typedef BOOL (WINAPI *SetProcInfoFn)(HANDLE, int, LPVOID, DWORD);
        HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        SetProcInfoFn setInfo = k32 ? reinterpret_cast<SetProcInfoFn>(reinterpret_cast<void*>(::GetProcAddress(k32, "SetProcessInformation"))) : nullptr;
        if (setInfo) {
            PowerThrottling st = { 1, 0x1 /*EXECUTION_SPEED*/ | 0x4 /*IGNORE_TIMER_RESOLUTION*/, 0 /*0 = nao limitar*/ };
            setInfo(::GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &st, sizeof(st));
        }
    }
    initClickers();
    loadCustomSounds();   // le .wav da pasta "sounds"
    buildSoundList();     // monta a lista (embutidos + custom)
    setSoundByName("Default");  // som padrao
    loadConfig();         // restaura a config salva (teclas, CPS, cor, som) -> sobrescreve o padrao

    HINSTANCE hInst = GetModuleHandle(nullptr);
    HICON hIcon = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        hInst, hIcon, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
        L"GNClicker", hIcon };
    ::RegisterClassExW(&wc);
    // janela de tamanho FIXO e SEM moldura: a barra de titulo e desenhada pelo proprio programa.
    int winW = (int)kWinW, winH = (int)kWinH;
    int posX = (::GetSystemMetrics(SM_CXSCREEN) - winW) / 2, posY = (::GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"GNClicker",
        WS_POPUP | WS_MINIMIZEBOX, posX, posY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);
    g_hwnd = hwnd;
    {   // Windows 11: cantos arredondados. Sombra: estende o quadro 1 px para dentro.
        DWORD corner = 2;   // DWMWCP_ROUND
        ::DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        MARGINS m = { 0, 0, 0, 1 };
        ::DwmExtendFrameIntoClientArea(hwnd, &m);
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nao criar/ler imgui.ini
    ImGui::StyleColorsDark();
    setupStyle();
    {   // fontes: Segoe UI do proprio Windows (se existir); senao a fonte vetorial embutida do ImGui
        char wd[MAX_PATH] = {0};
        ::GetWindowsDirectoryA(wd, MAX_PATH);
        std::string fr = std::string(wd) + "\\Fonts\\segoeui.ttf", fb = std::string(wd) + "\\Fonts\\segoeuib.ttf";
        ImFont* reg = (::GetFileAttributesA(fr.c_str()) != INVALID_FILE_ATTRIBUTES) ? io.Fonts->AddFontFromFileTTF(fr.c_str(), 15.0f) : nullptr;
        if (!reg) reg = io.Fonts->AddFontDefaultVector();
        g_fontBold = (::GetFileAttributesA(fb.c_str()) != INVALID_FILE_ATTRIBUTES) ? io.Fonts->AddFontFromFileTTF(fb.c_str(), 15.0f) : nullptr;
        if (!g_fontBold) g_fontBold = reg;
        ImGui::GetStyle().FontSizeBase = 15.0f;
    }
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    CreateLogoTexture();

    // threads de clique (esquerdo + direito) + hotkey + hook de mouse dedicado
    std::thread tL(clickWorker, &g_left);
    std::thread tR(clickWorker, &g_right);
    std::thread tHK(hotkeyWorker);
    std::thread tThrow(throwWorker);
    std::thread tWtap(wtapWorker);
    std::thread tMc(mcDetectWorker);
    std::thread tHook(mouseHookThread);

    const ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        drawUI();

        ImGui::Render();
        const float clear[4] = { clear_color.x, clear_color.y, clear_color.z, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0); // vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

    }

    // encerra threads
    g_running.store(false);
    if (tL.joinable()) tL.join();
    if (tR.joinable()) tR.join();
    if (tHK.joinable()) tHK.join();
    if (tThrow.joinable()) tThrow.join();
    if (tWtap.joinable()) tWtap.join();
    if (tMc.joinable()) tMc.join();
    // acorda a thread do hook (GetMessage) para ela encerrar e desinstalar o hook
    if (g_hookThreadId.load()) ::PostThreadMessageW((DWORD)g_hookThreadId.load(), WM_QUIT, 0, 0);
    if (tHook.joinable()) tHook.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    timeEndPeriod(1);
    return 0;
}

// ---- helpers DX11 (exemplo oficial do Dear ImGui - MIT) ---------------------
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    if (g_logoSRV) { g_logoSRV->Release(); g_logoSRV = nullptr; }
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Cria a textura da logo a partir dos pixels RGBA embutidos (logo_data.h).
void CreateLogoTexture() {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = LOGO_W; desc.Height = LOGO_H; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = LOGO_RGBA; sub.SysMemPitch = LOGO_W * 4;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex)) && tex) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = desc.Format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
        g_pd3dDevice->CreateShaderResourceView(tex, &srv, &g_logoSRV);
        tex->Release();
    }
}

// ---- icone na bandeja do sistema (bolinha verde esconde a janela, o clicker continua rodando) ----
static bool g_trayActive = false;

static void trayAdd() {
    if (g_trayActive) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    wcsncpy(nid.szTip, L"GNClicker", 63);
    g_trayActive = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}
static void trayRemove() {
    if (!g_trayActive) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayActive = false;
}
static void hideToTray() {
    trayAdd();
    if (g_trayActive) ::ShowWindow(g_hwnd, SW_HIDE);   // so esconde se o icone da bandeja existe (senao o usuario perderia a janela)
}
static void showFromTray() {
    ::ShowWindow(g_hwnd, SW_SHOW);
    ::ShowWindow(g_hwnd, SW_RESTORE);
    ::SetForegroundWindow(g_hwnd);
    trayRemove();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_HIDE_TO_TRAY:
        hideToTray();
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            showFromTray();
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt; ::GetCursorPos(&pt);
            HMENU menu = ::CreatePopupMenu();
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, g_lang == 1 ? L"Mostrar GNClicker" : L"Show GNClicker");
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, g_lang == 1 ? L"Sair" : L"Exit");
            ::SetForegroundWindow(hWnd);
            int cmd = (int)::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            ::DestroyMenu(menu);
            ::PostMessageW(hWnd, WM_NULL, 0, 0);
            if (cmd == ID_TRAY_SHOW) showFromTray();
            if (cmd == ID_TRAY_EXIT) ::PostMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_NCHITTEST: {   // arrastar a janela pela barra de titulo propria (menos nas bolinhas)
        LRESULT hit = ::DefWindowProcW(hWnd, msg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);
            RECT rc; ::GetClientRect(hWnd, &rc);
            if (pt.y >= 0 && pt.y < (int)kTitleH && pt.x > 90 && pt.x < rc.right - 4) return HTCAPTION;
        }
        return hit;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // desabilita ALT menu
        break;
    case WM_DESTROY:
        trayRemove();
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
