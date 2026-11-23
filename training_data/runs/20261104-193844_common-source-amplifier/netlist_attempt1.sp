* Common-source amplifier
.model nmos nmos(level=1 vto=0.5 kp=100u lambda=0.02 gamma=0.5 phi=0.6)

VDD vdd 0 1.8

* Gate bias divider
RG1 vdd gate 1.5M
RG2 gate 0 1M

* Input coupling
VIN in 0 SIN(0 10m 1k)
CIN in gate 1u

* MOSFET
M1 drain gate source 0 nmos w=20u l=1u

* Drain load
RD vdd drain 5k

* Source degeneration with AC bypass
RS source 0 1k
CS source 0 10u

* Output coupling
COUT drain out 1u
RL out 0 100k

* Initial condition to help convergence
.ic v(gate)=0.72 v(source)=0.2 v(drain)=0.8
