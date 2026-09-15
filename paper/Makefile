.PHONY: all figures clean
PYTHON ?= $(if $(wildcard ../.venv/bin/python),../.venv/bin/python,python3)
all: figures
	latexmk -xelatex -interaction=nonstopmode -halt-on-error main.tex
figures:
	$(PYTHON) make_figures.py
clean:
	latexmk -c main.tex
