all: extract_flow extract_packets synth_zipf_packets synth_zipf_seq

extract_flow: extract_flow.cpp *.h
	g++ -o extract_flow extract_flow.cpp -std=c++11

extract_packets: extract_packets.cpp *.h
	g++ -o extract_packets extract_packets.cpp -std=c++11

synth_zipf_packets: synth_zipf_packets.c ../utils/pkt-puller.h
	gcc -o synth_zipf_packets synth_zipf_packets.c -lm

synth_zipf_seq: synth_zipf_seq.c ../utils/pkt-puller.h
	gcc -o synth_zipf_seq synth_zipf_seq.c -lm

clean: 
	rm extract_flow extract_packets synth_zipf_packets
