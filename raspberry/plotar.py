"""
Gera o gráfico (temperatura medida x alvo, e potência do aquecedor)
a partir de um CSV criado pelo supervisor.py.
 
Uso:
  python3 plotar.py logs/forno_20261111_153000.csv
Saída: mesmo nome com extensão .png.
 
Instalação: pip install matplotlib  (ou: sudo apt install python3-matplotlib)
"""

import csv
import sys
from datetime import datetime
 
import matplotlib
import matplotlib.pyplot as plt
 
if len(sys.argv) < 2:
    print("Uso: python3 plotar.py <arquivo.csv>")
    sys.exit(1)
 
arquivo = sys.argv[1]
tempos, temp, alvo, pwm = [], [], [], []
 
with open(arquivo) as f:
    for linha in csv.DictReader(f):
        tempos.append(datetime.fromisoformat(linha["timestamp"]))
        temp.append(float(linha["t"]))
        alvo.append(float(linha["alvo"]))
        pwm.append(float(linha["pwm"]))
 
if not tempos:
    print("CSV vazio.")
    sys.exit(1)
 
# eixo x em segundos desde o início
t0 = tempos[0]
x = [(t - t0).total_seconds() for t in tempos]
 
fig, ax1 = plt.subplots(figsize=(10, 5))
ax1.plot(x, temp, label="Temperatura medida (°C)", linewidth=2)
ax1.plot(x, alvo, "--", label="Alvo (°C)")
ax1.set_xlabel("Tempo (s)")
ax1.set_ylabel("Temperatura (°C)")
ax1.grid(True, alpha=0.3)
 
ax2 = ax1.twinx()
ax2.fill_between(x, pwm, alpha=0.2, color="tab:red", label="Potência do aquecedor (%)")
ax2.set_ylabel("Potência (%)")
ax2.set_ylim(0, 110)
 
l1, n1 = ax1.get_legend_handles_labels()
l2, n2 = ax2.get_legend_handles_labels()
ax1.legend(l1 + l2, n1 + n2, loc="upper left")
plt.title("Forno inteligente - comportamento térmico")
plt.tight_layout()
 
saida = arquivo.rsplit(".", 1)[0] + ".png"
plt.savefig(saida, dpi=150)
print(f"Gráfico salvo em {saida}")
 
if matplotlib.get_backend().lower() != "agg":
    plt.show()