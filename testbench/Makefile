.PHONY: all test
all:
	bash scripts/build-input.sh
	sh scripts/build-observe.sh

test: all
	python3 -m unittest discover -s tests -v
