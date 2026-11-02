# Câmbio sequencial e freio de mão com PS Move

Um PlayStation Move (de PS3) vira câmbio sequencial ou alavanca de freio de mão para o volante Thrustmaster Ferrari 458 Spider no PS5. O Move fica preso num suporte. Ao inclinar o suporte, o driver aperta os botões de marcha (ou o do freio de mão) no **mesmo controle virtual do volante**, como se você puxasse as borboletas.

Funcionou no WRC e no Project CARS. Qualquer jogo que aceite as borboletas do volante deve funcionar.

## O que precisa

- PS5 desbloqueado rodando este driver do volante (`ps5-wheel-cambio-v7.elf`, veja o [README](README.md)).
- Volante Thrustmaster Ferrari 458 Spider no USB.
- PS Move (CECH-ZCM1 ou ZCM2) **pareado com o PS5 por Bluetooth**. Ligar o Move no cabo USB só carrega a bateria; a leitura é sempre por Bluetooth.
- Um suporte que segure o Move e deixe ele inclinar para frente e para trás. O protótipo usou o tubo do suporte de um projetor HY300, que já tem pivô lateral e níveis; o Move fica preso por dentro com espuma.

## Como usar

1. Rode o driver e abra a página de ajustes no celular: `http://IP-do-PS5:8088/`. Toque no atalho **Câmbio PS Move** no topo (no cockpit, é o botão **Câmbio**), ou abra `http://IP-do-PS5:8088/cambio`.
2. Ligue o Move (botão PS). Em poucos segundos a página mostra "PS Move conectado".
3. Deixe o tubo parado no meio: o centro é marcado sozinho. Para refazer, segure o botão **Move** por 1 s.
4. Empurre o tubo para frente. O ponteiro da página deve ir para o lado positivo. Se for para o lado errado, incline para frente e segure **Triângulo** por 1 s para ensinar qual é a frente.
5. Em **Usar o PS Move como**, escolha:
   - **Desligado (só mede):** o Move não manda nada ao jogo. Bom para testar o suporte.
   - **Câmbio:** cada nível para frente sobe uma marcha, cada nível para trás desce.
   - **Freio de mão:** puxar o tubo para trás segura o botão do freio; voltar para frente solta.
6. Escolha os botões e toque em **Salvar no PS5**. As opções ficam gravadas em `/data/ghostpad/move_shifter.txt`.

### Câmbio

- **Subir marcha / Descer marcha:** por padrão são os mesmos botões das borboletas do volante (aba Botões). Se o jogo usar outros botões para trocar de marcha, escolha-os aqui.
- **Testar subir / Testar descer:** apertam a troca pelo mesmo caminho do câmbio, sem mexer no tubo. Servem para descobrir se o problema é a leitura do Move ou o jogo.
- **Modo rápido:** troca no meio do movimento, sem esperar o tubo parar. Nas primeiras 3 trocas ele aprende sozinho o sentido do giroscópio (troque devagar, esperando o tubo parar em cada nível); depois a página mostra "modo rápido ativo".
- **Degrau mínimo:** quanto o tubo precisa andar entre um nível e outro para contar como troca. No cockpit do protótipo, 3° funcionou.
- **Freio de mão (gatilho T):** no modo câmbio, puxar o gatilho T do Move segura o botão escolhido.

### Freio de mão

- **Botão do freio de mão:** o botão que o jogo usa para o freio de mão.
- **Puxar para trás a partir de:** ângulo que aciona o freio (padrão 8°). Ele só solta quando o tubo volta além da metade desse ângulo, para não piscar com a mão tremendo.
- **Segurar junto com o puxão:** exige um botão do Move apertado junto (gatilho T, Cruz, Círculo ou Quadrado). Assim, se o Move cair para trás sozinho, o freio não aciona. Move e Triângulo ficam de fora porque segurá-los por 1 s recentraliza e ensina a frente.

## Como funciona

### 1. Ler o Move pelo Bluetooth do PS5

O PS5 não entrega o Move aos jogos, mas o driver de HID do sistema (`/dev/hid`) deixa abrir os aparelhos Bluetooth pareados. O driver testa os canais 1 a 8 (o 0 é o DualSense), e fica com o que manda relatórios com a marca fixa do Move (`7F 7F 7F 7F`) e um acelerômetro perto de 1 g. Cada relatório traz os botões, o gatilho T e **duas amostras** de acelerômetro e giroscópio (cerca de 175 amostras por segundo). O LED da bola é reenviado a cada 0,5 s, porque o Move apaga se não receber nada.

Para não mexer em outros controles Bluetooth, o driver só procura o Move quando a opção está ligada ou quando a página `/cambio` está aberta.

### 2. Transformar em inclinação

O ângulo vem do **acelerômetro**, que mede a direção da gravidade. O centro é a posição parada no meio; a inclinação é o ângulo da gravidade para frente ou para trás em relação a esse centro. Para não confundir tranco com inclinação:

- a leitura é suavizada (cerca de 50 ms);
- leituras longe de 1 g (pancada, tranco da mão) são ignoradas;
- o desvio lento do sensor é corrigido: se o tubo fica parado perto do meio por 2 s, aquele ponto vira o novo centro.

Usar o giroscópio para calcular o ângulo **não funcionou**: no Move, os eixos dele não batem com os do acelerômetro, e o ângulo dava saltos de ±50°.

### 3. Reconhecer a troca

- **Modo nível (padrão):** o suporte do projetor trava em níveis e não volta sozinho ao meio. Então a troca não é "distância do centro", e sim **mudar de um nível parado para outro**. Quando o tubo fica parado por 0,18 s num nível pelo menos um degrau à frente do anterior, sobe uma marcha; atrás, desce. Trancos e balanços nunca param, então não contam. Depois de uma troca, por 0,35 s um novo assentamento só atualiza o nível, sem contar outra troca, porque o tubo quica ao chegar no nível.
- **Modo rápido:** o giroscópio mede a rotação na hora do empurrão. Como os eixos dele não batem com os do acelerômetro, o driver **aprende** o eixo de rotação do pivô: em cada troca no modo nível, guarda para que lado o giroscópio girou. Depois de 3 trocas coerentes, passa a disparar assim que a rotação nesse eixo passa de 40°/s e já andou 60% do degrau, cerca de 30 ms após o início do empurrão. A próxima troca só é liberada quando o tubo para, e é isso que impede o quique de virar segunda troca. Se duas trocas saírem com o sentido errado (por exemplo, o Move girou dentro da espuma), ele desaprende e volta ao modo nível.

Os tempos foram ajustados no uso: esperar 0,3 s parado dava atraso, e 0,12 s fazia trocas duplas.

### 4. Apertar o botão no jogo

O volante já tem um controle virtual (DualSense) alimentado pelo driver. A troca não cria outro controle: o driver pega o último estado do volante, acrescenta o botão da marcha e reenvia por `scePadVirtualDeviceInsertData`, segurando o botão por 150 ms. Enquanto o botão está apertado, os relatórios normais do volante também saem com ele, para nenhum relatório "soltar" o botão antes da hora. O reenvio mantém o mesmo carimbo de tempo do relatório do volante: quando ele ia com o relógio do sistema, o Project CARS ignorava o aperto.

## Limitações

- O suporte do HY300 não tem mola. Voltar ao centro conta como troca (é mudar de nível). Com um suporte com mola que volte ao meio, o comportamento ideal seria "saiu do centro = troca, voltou = libera".
- O centro e o eixo do modo rápido são aprendidos de novo a cada vez que o driver inicia.
- O freio de mão é digital (um botão), não analógico.
- Só um Move é lido por vez.

## Arquivos

| Arquivo | O que faz |
|---|---|
| `payload/controller_move.c/.h` | Lê o relatório Bluetooth do Move |
| `payload/shifter.c/.h` | Inclinação, troca por nível, modo rápido, freio de mão, opções salvas |
| `payload/gc_main.c` (`move_shifter_thread`) | Conecta o Move e aperta os botões no controle virtual do volante |
| `payload/controller_wheel.c` (`wheel_compose_pad`) | Junta o botão extra ao estado do volante |
| `payload/web_server.c` (`/api/shifter…`) | API da página |
| `payload/web/shifter.html` | Página `/cambio` |
| `tests/test_shifter.c` | Testes do câmbio, com movimentos simulados |

Os testes rodam no Linux ou WSL com `bash tests/run_tests.sh`. Eles simulam o Move e não substituem o teste no console.
