<div align="center">

# GNClicker

**Um autoclicker pequeno, rápido e transparente para Minecraft no Windows.**
Feito no Brasil 🇧🇷 por [Gazake](https://github.com/Gazake1).

**[Site e download](https://gazake1.github.io/gnclicker-site/?lang=pt)** · **[Releases](https://github.com/Gazake1/GNClicker/releases)** · [English](README.md)

</div>

---

## O que faz

O GNClicker aperta o botão do mouse por você, na velocidade que escolher (1 a 100 cliques por segundo), enquanto você
segura o botão. Arme com uma tecla de atalho, segure o botão do mouse e ele clica enquanto você segurar. Foi pensado
para Minecraft, mas o núcleo funciona em qualquer janela.

- **Clicker:** lado esquerdo e direito com tecla e CPS próprios, *Inventory*, *Blockhit*, *Jitter* (só no clique esquerdo, até 15, **e só quando o Minecraft ou um launcher/client conhecido está na janela ativa**; o auto click funciona em qualquer programa) e janela alvo.
- **Macros:** *Slot Whitelist* (com *Fetch Binds*), *Throwpot* e *W-Tap*.
- **Config:** 8 sons de mouse embutidos, 4 sintetizados e qualquer `.wav` na pasta `sounds`; cor do tema e modo Rainbow.
  Tudo é salvo em `%APPDATA%\GNClicker\config.txt`.

## Transparência

O que ele **não** faz: não acessa a memória de outros processos, não usa a rede, não se instala (sem registro nem
tarefas agendadas), não se auto-deleta, não roda comandos de shell e não apaga rastros. Usa só APIs padrão do Windows
(`mouse_event`/`SendInput`, `GetAsyncKeyState` só para as teclas configuradas, um hook de mouse só para estado dos
botões e scroll, `GetCursorInfo`, `GetForegroundWindow`, `PlaySound` e `Shell_NotifyIcon` para o ícone da bandeja). **Não existe hook de teclado.**
Para saber se o Minecraft está em foco (só para o Jitter), ele lê o **título e o nome do arquivo do programa da janela ativa**
(por exemplo `javaw.exe`) e mais nada dos outros programas: nunca lê a memória deles. Ele nunca clica enquanto a janela
do próprio GNClicker está ativa.
Dicas aparecem ao parar o mouse ~3 s sobre o nome de uma função; a bolinha verde esconde o programa na bandeja; e em
*Settings* dá para trocar o modo de ativação entre *segurar o botão* (padrão) e *apertar uma vez*.

> [!WARNING]
> Alguns antivírus marcam qualquer autoclicker como PUA/HackTool, mesmo sem malware. Blockhit, jitter, throwpot e W-tap
> são mais fáceis de detectar por anticheats, e muitos servidores de Minecraft proíbem autoclickers. Use por sua conta e
> risco e confira as regras do seu servidor.

## Download

Baixe em **[Releases](https://github.com/Gazake1/GNClicker/releases)** (o SHA-256 de cada arquivo está na descrição).
`GNClicker.exe` é a versão normal; `GNClicker-Admin.exe` é para quando o jogo ou o launcher roda como administrador.

Se o Windows mostrar "fornecedor desconhecido" (SmartScreen), clique em **Mais informações → Executar assim mesmo**, ou
clique com o botão direito no arquivo → **Propriedades → Desbloquear**.

## Compilar

```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT
build.bat
```

O `build.bat` baixa o Dear ImGui em um **commit fixo** (build reproduzível), compila com MinGW-w64 e gera o
`GNClicker.exe` e o `GNClicker-Admin.exe`. As releases são compiladas pelo GitHub Actions a partir deste repositório.

## Política de assinatura de código

> Free code signing provided by [SignPath.io](https://signpath.io), certificate by
> [SignPath Foundation](https://signpath.org).

**Situação:** um certificado gratuito está sendo solicitado à SignPath Foundation. Enquanto não for concedido, as releases **não são assinadas**.
Autor, revisor e aprovador: [Gazake](https://github.com/Gazake1) (único mantenedor). Política de privacidade: este
programa não transfere nenhuma informação para outros sistemas em rede, a menos que o usuário peça (ele não usa rede).

## Licença

[MIT](LICENSE) © 2026 Gazake. Avisos de terceiros em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
