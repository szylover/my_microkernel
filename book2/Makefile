# ============================================================================
# 《如何用 AI Agent 编写自己的操作系统》 LaTeX 构建
# ============================================================================
#
# 用法：
#   make          # 编译 PDF（两遍 xelatex 生成目录）
#   make once     # 只跑一遍（快速预览）
#   make clean    # 清理构建产物

MAIN = main
ENGINE = xelatex
FLAGS = -interaction=nonstopmode -halt-on-error -shell-escape

.PHONY: all once clean

all: $(MAIN).pdf

$(MAIN).pdf: $(MAIN).tex preamble.tex chapters/*.tex
	$(ENGINE) $(FLAGS) $(MAIN).tex
	$(ENGINE) $(FLAGS) $(MAIN).tex

once:
	$(ENGINE) $(FLAGS) $(MAIN).tex

clean:
	rm -f *.aux *.log *.out *.toc *.lof *.lot *.fls *.fdb_latexmk \
	      *.synctex.gz *.bbl *.blg *.bcf *.run.xml *.idx *.ilg *.ind *.xdv \
	      *.listing
	rm -f chapters/*.aux
