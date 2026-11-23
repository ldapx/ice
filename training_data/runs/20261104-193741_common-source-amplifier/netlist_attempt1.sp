* Common-source amplifier
VDD vdd 0 1.8
Vin in 0 SIN(0 5m 1k)

* Bias network
R1 vdd gate 1.2Meg
R2 gate 0 300k

* Drain load
RD vdd drain 47k

* Source degeneration with AC bypass
RS source 0 1k
Cs source 0 1u

* MOSFET
M1 drain gate source 0 nmos W=20u L=1u

* Coupling capacitors
Cin in gate 10n
Cout drain out 10n

* Load resistor (100 kΩ to ground)
RL out 0 100k

.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.7)

.ic v(gate)=0.9 v(source)=0.4 v(drain)=1.2
