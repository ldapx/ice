* Common-source amplifier
VDD vdd 0 1.8
Vin in 0 SIN(0 10m 1k)

* Bias network
RG1 vdd gate 1M
RG2 gate 0 1M

* Input coupling
Cin in gate 1u

* MOSFET
M1 drain gate source 0 nmos W=22.2u L=1u

* Load and source degeneration
RD vdd drain 10k
RS source 0 1k
CS source 0 10u

* Output coupling
Cout drain out 1u
RL out 0 100k

.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)

.ic v(gate)=0.9 v(source)=0.1 v(drain)=0.8
