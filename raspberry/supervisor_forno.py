#!/usr/bin/env python3
"""
Supervisor do forno inteligente - roda na Raspberry Pi.

Funções:
  - lê a telemetria JSON enviada pelo ESP32 (thread de leitura)
  - grava cada amostra em um arquivo CSV (log)
  - envia PING a cada 1 s (heartbeat, usado pelo fail-safe do ESP32)
  - aceita comandos digitados no terminal (thread principal)

Comandos no terminal:
  set <temp>   define o alvo pela Raspberry (ex.: set 150)
  pot          volta a usar o potenciometro como alvo
  start        liga o forno
  stop         desliga o forno
  sair         encerra o programa

Uso:
  python3 supervisor.py                 (porta padrão /dev/ttyUSB0)
  python3 supervisor.py /dev/ttyACM0    (outra porta)
"""

import csv
import json
import os
import sys
import threading
import time
from datetime import datetime

import serial  # pip install pyserial  (ou: sudo apt install python3-serial)

PORTA = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BAUD = 115200

encerrar = threading.Event()
trava_serial = threading.Lock()  # evita duas threads escrevendo ao mesmo tempo


def enviar(ser, texto):
    """Envia uma linha de comando ao ESP32."""
    with trava_serial:
        ser.write((texto + "\n").encode())


def thread_leitura(ser, arquivo_csv):
    """Lê linhas JSON do ESP32, mostra no terminal e grava no CSV."""
    with open(arquivo_csv, "w", newline="") as f:
        escritor = csv.writer(f)
        escritor.writerow(["timestamp", "t", "alvo", "pwm", "estado", "modo"])

        while not encerrar.is_set():
            try:
                linha = ser.readline().decode(errors="ignore").strip()
            except serial.SerialException:
                print("Erro na porta serial.")
                encerrar.set()
                break

            if not linha.startswith("{"):
                continue  # ignora lixo / mensagens de boot do ESP32
            try:
                d = json.loads(linha)
            except json.JSONDecodeError:
                continue

            agora = datetime.now().isoformat(timespec="milliseconds")
            escritor.writerow([agora, d["t"], d["alvo"], d["pwm"], d["estado"], d["modo"]])
            f.flush()

            print(
                f"[{agora[11:19]}] T={d['t']:6.1f} °C | alvo={d['alvo']:5.0f} °C | "
                f"aquecedor={d['pwm']:3.0f}% | {d['estado']:<10} | alvo via {d['modo']}"
            )


def thread_heartbeat(ser):
    """Envia PING a cada segundo para o ESP32 saber que a Pi está viva."""
    while not encerrar.is_set():
        enviar(ser, "PING")
        time.sleep(1)


def main():
    os.makedirs("logs", exist_ok=True)
    arquivo_csv = os.path.join("logs", datetime.now().strftime("forno_%Y%m%d_%H%M%S.csv"))

    try:
        ser = serial.Serial(PORTA, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"Não foi possível abrir {PORTA}: {e}")
        print("Dica: veja a porta com  ls /dev/ttyUSB* /dev/ttyACM*")
        sys.exit(1)

    time.sleep(2)  # o ESP32 reinicia quando a porta é aberta
    print(f"Conectado em {PORTA}. Log em {arquivo_csv}")
    print("Comandos: set <temp> | pot | start | stop | sair\n")

    threading.Thread(target=thread_leitura, args=(ser, arquivo_csv), daemon=True).start()
    threading.Thread(target=thread_heartbeat, args=(ser,), daemon=True).start()

    try:
        while not encerrar.is_set():
            cmd = input().strip().lower()
            if cmd == "sair":
                break
            elif cmd.startswith("set "):
                try:
                    valor = float(cmd.split()[1])
                    enviar(ser, f"SET {valor:.0f}")
                except (IndexError, ValueError):
                    print("Uso: set <temperatura>")
            elif cmd in ("pot", "start", "stop"):
                enviar(ser, cmd.upper())
            elif cmd:
                print("Comando desconhecido.")
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        encerrar.set()
        time.sleep(0.3)
        ser.close()
        print("Encerrado.")


if __name__ == "__main__":
    main()