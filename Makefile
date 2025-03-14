build:
	mpic++ -o tema2 tema2.cpp -pthread -Wall

clean:
	rm -rf tema2
	rm -rf tema2 client*_*
