# ============================================================================
# 《自己动手写操作系统》 LaTeX 构建
# ============================================================================
#
# 用法：
#   make          # 编译 PDF（两遍 xelatex 生成目录）
#   make once     # 只跑一遍（快速预览）
#   make clean    # 清理构建产物
#   make watch    # 持续监听文件变化自动重编译（需要 latexmk）
#
# 依赖：
#   sudo apt install texlive-xetex texlive-lang-chinese texlive-latex-extra \
#        texlive-fonts-recommended texlive-science latexmk

MAIN = main
ENGINE = xelatex
FLAGS = -interaction=nonstopmode -halt-on-error -shell-escape

.PHONY: all once clean watch

all: $(MAIN).pdf

$(MAIN).pdf: $(MAIN).tex preamble.tex chapters/*.tex
	$(ENGINE) $(FLAGS) $(MAIN).tex
	$(ENGINE) $(FLAGS) $(MAIN).tex

once:
	$(ENGINE) $(FLAGS) $(MAIN).tex

clean:
	rm -f *.aux *.log *.out *.toc *.lof *.lot *.fls *.fdb_latexmk \
	      *.synctex.gz *.bbl *.blg *.bcf *.run.xml *.idx *.ilg *.ind *.xdv
	rm -f chapters/*.aux

watch:
	latexmk -pvc -$(ENGINE) -interaction=nonstopmode $(MAIN).tex
