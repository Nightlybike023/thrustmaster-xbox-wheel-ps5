# Testes locais do volante

No WSL/Linux, execute `bash tests/run_tests.sh` dentro de `ghostcontrol`.
Requer compilador C, Python 3 e suporte a AddressSanitizer/UndefinedBehaviorSanitizer.
O mesmo comando funciona no diretório `CODIGO_FONTE` da distribuição.

Teste opcional da interface: com Playwright disponível e Chrome instalado,
execute `node tests/test_ui.cjs`. Usa respostas de API simuladas em navegador
headless e verifica celular, paisagem, zoom, falhas de salvamento e telemetria.
Também verifica calibração guiada, leituras antigas, perfis, exportação/importação,
restauração dos padrões, diagnóstico e ajuste do contador de marcha.

Os testes executam os arquivos reais `controller_wheel.c`, `web_server.c` e
`usb_helpers.c`, com USB simulado e arquivos de configuração temporários.
Cobrem limites dos pacotes, continuidade do botão Guide com pedais e direção,
troca de marchas, reconexão, mapeamento de gatilhos, validação e persistência,
leituras concorrentes, erros USB e requisições HTTP fragmentadas.
As extensões cobrem direção assimétrica, pedais calibrados, curvas, sensibilidade,
suavização, oito perfis persistentes, backup atômico e falhas de gravação.
Também cobrem atividade do botão Guide após uma pausa, preservação de códigos
de erro USB, nomes como GT7 na importação e recusa de uma segunda porta web.

A instrumentação de botões é coberta por bordas de acionamento e soltura,
resultado do envio (aceito, recusado ou não enviado), duração dos toques,
lacunas e repetições na sequência GIP (inclusive a virada 255→1), pacotes
curtos ou de outros tipos, esperas USB, buffer circular de eventos e zerar
contadores. `test_http.py` também abre um servidor com o buffer cheio para
validar o JSON de `/api/buttons` no pior caso.

`test_instance.c` usa processos de teste no Linux para verificar exclusão de
instâncias, substituição do dono atual da trava e recusa de caminhos inválidos.
Um número de processo antigo gravado no arquivo não causa sinalização de outro
processo. Esses testes não encerram processos do usuário ou do console.

As páginas editáveis ficam em `payload/web/` (ou `web/` na distribuição).
`embed_web.py` gera `web_ui.h`; o Makefile e o teste local fazem essa geração
automaticamente. Python 3 é necessário para compilar quando as páginas mudam.

Os cabeçalhos em `compat/` permitem testar a lógica no Linux. A compatibilidade
com a ABI do USB do PS5 é verificada na compilação com o SDK real. Os testes
locais não comprovam o funcionamento do volante no console e não acessam o PS5.

`test_shifter.c` simula o PS Move num suporte com níveis: troca por nível,
quique depois da troca, trancos sem rotação, modo rápido (aprendizado do eixo,
disparo no meio do movimento, desaprender quando o Move gira no suporte), freio
de mão com histerese e botão obrigatório, opções salvas e varredura do Bluetooth
só com a opção ligada ou a página aberta. `test_wheel.c` cobre o botão extra
junto ao relatório do volante e `test_http.py` a API `/api/shifter`.
