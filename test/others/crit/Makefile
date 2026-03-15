run: clean
	@make -C .. loop
	./test.sh

clean:
	rm -f *.img *.log *.txt stats-* *.json
	rm -rf comp/ uncomp/ cdc/ dcd/ sparse-compress/ sparse-decompress/ threshold-compress/ truncated-decompress/
