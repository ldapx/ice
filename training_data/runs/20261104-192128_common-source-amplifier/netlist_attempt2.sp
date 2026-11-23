* Common-source amplifier with PMOS active load
VDD vdd 0 1.8
Vin in 0 SIN(0 10m 1k)

* NMOS bias divider
R1 vdd gate 33k
R2 gate 0 100k

* NMOS source degeneration with AC bypass
RS source 0 1k
CS source 0 10u

* Input coupling capacitor
Cin in gate 1u

* PMOS current mirror load
Rref bias 0 2k
M2 bias bias vdd vdd pmos
M3 drain bias vdd vdd pmos

* NMOS amplifier
M1 drain gate source 0 nmos

* Output coupling and high-impedance load
Cout drain out 1u
RL out 0 1Meg

.model nmos nmos(level=1 vto=0.5 kp=1m lambda=0.02 gamma=0.5 phi=0.7)
.model pmos pmos(level=1 vto=-0.5 kp=0.5m lambda=0.02 gamma=0.5 phi=0.7)

.ic v(gate)=1.35 v(source)=0.2 v(drain)=0.9 v(bias)=0.4
