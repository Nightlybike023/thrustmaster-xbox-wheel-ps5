# Volante Thrustmaster de Xbox no PS5 + câmbio PS Move

Driver para usar o volante **Thrustmaster Ferrari 458 Spider** (volante de Xbox One) num **PS5 desbloqueado**. Tem também uma página de ajustes no celular, um cockpit ao vivo e um **câmbio sequencial / freio de mão feito com um PS Move**.

Este projeto é baseado no **[Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher), de StonedModder**. A seção [Créditos](#créditos-e-o-que-veio-do-ghostcontrol) explica com números o que veio de lá e o que é novo.

> *English summary at the end.*

---

## O que faz

- O PS5 passa a aceitar o volante: direção, acelerador, freio, borboletas e todos os botões.
- O jogo enxerga um **DualSense comum**, então funciona em qualquer jogo de corrida, sem suporte oficial a volante.
- **Página de ajustes** no celular (`http://IP-DO-PS5:8088`): calibração guiada, zona morta, sensibilidade, suavização, mapeamento de botões, 8 perfis por jogo, backup e diagnóstico.
- **Cockpit ao vivo** (`/overlay`): conta-giros, marcha, volante e pedais. Dá para escolher a cor do mostrador, esconder partes e trocar os lados.
- **Câmbio PS Move** (`/cambio`): um PS Move preso num suporte vira câmbio sequencial ou alavanca de freio de mão. Veja [CAMBIO_PS_MOVE.md](CAMBIO_PS_MOVE.md).

Testado no firmware **13.60** com WRC e Project CARS.

## O que precisa

- PS5 desbloqueado, com um carregador de payloads (ProsperoMgr, Payload Manager ou elfldr).
- Volante Thrustmaster Ferrari 458 Spider ligado no USB do PS5.
- Opcional: PS Move (de PS3) pareado com o PS5 por Bluetooth, para o câmbio.
- Celular ou computador na mesma rede do PS5, para as páginas.

---

## Como baixar e instalar

### 1. Baixe o arquivo

Baixe **[`ps5-wheel-cambio-v7.elf`](https://github.com/Nightlybike023/volante-thrustmaster-xbox-ps5/raw/main/dist/ps5-wheel-cambio-v7.elf)**, que está na pasta [`dist/`](dist/) deste repositório.

Se quiser conferir que o download veio inteiro, o SHA-256 é:

```
554bf897692d3a4ae3aae56764fccd1da597cf7378eac6f8041f74f7d89efd02
```

### 2. Mande para o PS5

Use o carregador que você já tem. Qualquer uma destas formas serve:

- **Payload Manager:** abra o Payload Manager pelo navegador do celular (`http://IP-DO-PS5:8084`), vá em **Gerenciar payloads** e envie o arquivo. Ele passa a aparecer na lista para abrir quando quiser.
- **ProsperoMgr:** abra `http://IP-DO-PS5:7070`, envie o `.elf` em Payloads e toque em iniciar.
- **Pendrive:** copie o `.elf` para um pendrive. Os gerenciadores que leem USB mostram o arquivo para abrir.
- **elfldr (porta 9021), pelo computador:**

  ```sh
  nc -w 5 IP-DO-PS5 9021 < ps5-wheel-cambio-v7.elf
  ```

Abra **só um** driver de volante por vez. Se você tiver outra versão deste driver, feche a outra antes.

### 3. Ligue o volante e abra a página

1. Ligue o volante no USB do PS5. Aparece o aviso "volante ativo" e ele já funciona no jogo.
2. No celular, abra `http://IP-DO-PS5:8088`. O IP do PS5 está em *Configurações → Rede → Status da conexão*.
3. Na primeira vez, faça a **calibração guiada** e toque em **Salvar no PS5**.

Os ajustes ficam salvos no PS5, em `/data/ghostpad/`. O payload precisa ser aberto de novo sempre que o PS5 é ligado, como qualquer payload.

---

## Como funciona

O PS5 não reconhece o Ferrari 458 Spider: ele é um volante de **Xbox One** e fala o protocolo do Xbox (**GIP**). O driver é um programa que roda dentro do PS5 desbloqueado e faz a ponte:

1. **Assume a porta USB do volante.** Tira o volante do controle do sistema e passa a falar direto com ele.
2. **"Acorda" o volante.** Volante de Xbox fica mudo até receber a sequência de inicialização do Xbox. O driver manda essa sequência e confirma cada pacote, como o Xbox faria.
3. **Cria um DualSense virtual.** Usa funções internas do próprio PS5 para criar um controle "de mentira" e ligá-lo ao usuário. Esta parte vem do Ghostcontrol.
4. **Traduz o volante para o DualSense:**
   - a direção vai para o analógico esquerdo;
   - o acelerador vai para o R2 e o freio para o L2;
   - as borboletas e os botões vão para os botões que você escolher.
   Calibração, zona morta e suavização são aplicadas antes.
5. **Mantém o controle "vivo".** Parado, o volante só manda um pacote por mudança. O driver deixa a leitura do USB sempre aberta, para não perder nenhum, e repete o último estado ao PS5 a cada 50 ms, como um controle de verdade. Se o "soltou" do botão Xbox/PS se perder, o PS é solto sozinho depois de 2 s.
6. **Servidor web embutido** na porta 8088, com as páginas de ajustes, cockpit e câmbio.

O câmbio PS Move lê o Move pelo Bluetooth do PS5 e calcula a inclinação pelo acelerômetro. Ele aperta o botão de marcha **no mesmo DualSense virtual do volante**, por isso o jogo aceita: continua vendo um controle só. Os detalhes estão em [CAMBIO_PS_MOVE.md](CAMBIO_PS_MOVE.md).

**Limitações:**
- Não tem force feedback: o driver só lê o volante, não manda força para ele.
- Feito e testado com o Ferrari 458 Spider. Outros volantes de Xbox One podem precisar de ajustes no código.

---

## Créditos e o que veio do Ghostcontrol

### Ghostcontrol — StonedModder

**[Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher)**, de **StonedModder**, é a base deste projeto. Ele resolveu a parte mais difícil:
- criar um DualSense virtual no PS5;
- ligá-lo ao usuário certo pela ShellUI;
- assumir um controle USB.

Sem isso, este driver não existiria.

Este repositório foi feito **a partir do repositório do Ghostcontrol, mantendo o histórico**. Os primeiros commits são os originais, com os autores originais (StonedModder / ItsDeidara), e dá para ver no `git log` e no `git blame` exatamente o que foi mudado depois. A licença continua a mesma, **GPL-3.0-or-later**.

Contagem de linhas pelo `git blame`, no código C do payload (8.927 linhas no total):

| Parte | Arquivos | Origem |
|---|---|---|
| DualSense virtual e ligação ao usuário | `shellui_pad.c/.h` | **Ghostcontrol**, praticamente sem mudanças (4.064 de 4.077 linhas) |
| Programa principal: controle virtual, detecção USB, envio | `gc_main.c` | **Ghostcontrol** como base (693 de 1.099 linhas); novos o volante no laço USB, a leitura sem cancelamento, o câmbio PS Move e o servidor web |
| Funções USB e tipos | `usb_helpers.c/.h`, `gc_types.h` | **Ghostcontrol** (102 de 135 linhas) |
| Controles Switch Pro e Xbox Series | `controller_nintendo.*`, `controller_xbox.*` | **Ghostcontrol** (mantidos; o volante não usa) |
| Driver do volante 458 Spider (GIP, calibração, perfis, diagnóstico) | `controller_wheel.c/.h` | **Novo** (1.020 linhas) |
| Câmbio e freio de mão PS Move | `shifter.c/.h`, `controller_move.c/.h` | **Novo** (690 linhas) |
| Servidor web e API | `web_server.c/.h`, `embed_web.py` | **Novo** |
| Trava de instância única | `instance_guard.c/.h` | **Novo** |
| Páginas: ajustes, cockpit, câmbio, demo | `payload/web/`, `index.html`, `overlay-demo.html` | **Novo** |
| Testes automáticos (C, HTTP, interface) | `tests/` | **Novo** |

Em resumo: **cerca de 59% das linhas em C vêm do Ghostcontrol**, e quase tudo isso está num único arquivo, o `shellui_pad.c`, com 4 mil linhas. Fora esse arquivo, o Ghostcontrol responde por cerca de 25% do código C. O suporte ao volante, o câmbio PS Move, o servidor web, as páginas e os testes foram escritos para este projeto.

Se quiser apoiar o autor original, o endereço BTC que ele divulga no Ghostcontrol é `bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5`.

### XboxWheelCompatibility — camren-m

**[XboxWheelCompatibility](https://github.com/camren-m/XboxWheelCompatibility)**, de **Camren Mumme (camren-m)**, faz volantes de Xbox sem suporte, testado justamente com o Ferrari 458 Spider, funcionarem no Windows. Ele lê o volante e entrega ao sistema como um controle comum.

Foi a referência de que essa ideia funciona com este volante. **Nenhum código dele foi usado**: o projeto dele é em C# para Windows e usa as APIs do próprio Windows, enquanto este é em C, roda dentro do PS5 e fala o protocolo USB do volante diretamente. O XboxWheelCompatibility é licenciado sob MIT.

### Outros

- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), usado para compilar.
- [NoSleep.js](payload/web/vendor/README.md) (MIT), para manter a tela do celular ligada no cockpit.

Volante, câmbio PS Move e páginas: **soldado ([Nightlybike023](https://github.com/Nightlybike023))**.

---

## Compilar

```sh
export PS5_PAYLOAD_SDK=/caminho/para/ps5-payload-sdk
cd payload
make TARGET=ps5-wheel-cambio.elf
```

Testes (Linux ou WSL): `bash tests/run_tests.sh`. Os testes de interface usam Playwright; veja [tests/README.md](tests/README.md).

Para ver o cockpit sem PS5, abra o [`overlay-demo.html`](overlay-demo.html) no navegador.

Documentação herdada do Ghostcontrol, para outros controles USB: [ProControllerResearch.md](ProControllerResearch.md), [othercontrollersGuide.md](othercontrollersGuide.md) e [xboxSeriesSButtonBits.md](xboxSeriesSButtonBits.md).

## Licença

**GPL-3.0-or-later**, a mesma do Ghostcontrol. Veja [LICENSE](LICENSE). Você pode usar, modificar e redistribuir, desde que mantenha a mesma licença e publique o código das suas mudanças.

---

## English summary

Driver for the **Thrustmaster Ferrari 458 Spider** (an Xbox One GIP wheel) on a **jailbroken PS5**, with a phone settings page, a live cockpit dashboard and a **PS Move sequential shifter / handbrake**. The game sees a regular DualSense.

- **Download:** [`dist/ps5-wheel-cambio-v7.elf`](https://github.com/Nightlybike023/volante-thrustmaster-xbox-ps5/raw/main/dist/ps5-wheel-cambio-v7.elf). Load it with your payload loader (Payload Manager, ProsperoMgr or `nc IP 9021`), plug in the wheel, then open `http://PS5-IP:8088`.
- **Built on [Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher) by StonedModder.** It provides the virtual DualSense, the user binding and the USB takeover: about 59% of the C lines, mostly `shellui_pad.c`. The original history and authors are kept in this repository.
- **New in this project:** the GIP wheel driver, the PS Move shifter, the web server and pages, and the tests.
- **[XboxWheelCompatibility](https://github.com/camren-m/XboxWheelCompatibility) by camren-m** (Windows, MIT) showed this wheel works when it is read and re-injected as a standard gamepad. None of its code is used here.
- **License:** GPL-3.0-or-later.
