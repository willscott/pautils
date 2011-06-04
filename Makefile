all: pastat patogglepid

patogglepid : patogglepid.c
	gcc -Wall -o patogglepid patogglepid.c -lpulse

pastat : pastat.c
	gcc -Wall -o pastat pastat.c -lpulse
