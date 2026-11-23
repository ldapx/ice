* Common-source amplifier
VDD vdd 0 1.8
Vin in 0 SIN(0 10m 1k)

* Bias network
R1 vdd gate 100k
R2 gate 0 200k

* Source degeneration with AC bypass
RS source 0 1k
CS source 0 10u

* Drain load
RD vdd drain 10k

* Load resistor
RL out 0 10k

* Coupling capacitors
Cin in gate 1u
Cout drain out 1u

* NMOS transistor
M1 drain gate source 0 nmos

.model nmos nmos(level=1 vto=0.5 kp=1m lambda=0.02 gamma=0.5 phi=0.7)

.ic v(gate)=1.2 v(source)=1.05 v(drain)=1.65
