# StarPU --- Runtime system for heterogeneous multicore architectures.
#
# Copyright (C) 2009, 2011, 2013-2014  Université de Bordeaux
# Copyright (C) 2010, 2011, 2012, 2013, 2014, 2015  Centre National de la Recherche Scientifique
#
# Permission is granted to copy, distribute and/or modify this document
# under the terms of the GNU Free Documentation License, Version 1.3
# or any later version published by the Free Software Foundation;
# with no Invariant Sections, no Front-Cover Texts, and no Back-Cover Texts.
#
# See the GNU Free Documentation License in COPYING.GFDL for more details.


DOXYGEN = doxygen
PDFLATEX = pdflatex
MAKEINDEX = makeindex

DOX_DIR = $(top_builddir)/doc/doxygen
DOX_CONFIG = $(top_srcdir)/doc/doxygen/doxygen.cfg

DOX_HTML_DIR = html
DOX_LATEX_DIR = latex
DOX_PDF = starpu.pdf
DOX_TAG = starpu.tag

chapters =	\
	chapters/00introduction.doxy \
	chapters/01building.doxy \
	chapters/02basic_examples.doxy \
	chapters/03advanced_examples.doxy \
	chapters/04optimize_performance.doxy \
	chapters/05performance_feedback.doxy \
	chapters/06tips_and_tricks.doxy \
	chapters/08mpi_support.doxy \
	chapters/09fft_support.doxy \
	chapters/11c_extensions.doxy \
	chapters/12socl_opencl_extensions.doxy \
	chapters/13scheduling_contexts.doxy \
	chapters/14scheduling_context_hypervisor.doxy \
	chapters/15environment_variables.doxy \
	chapters/16configure_options.doxy \
	chapters/17files.doxy \
	chapters/18scaling-vector-example.doxy \
	chapters/19fdl-1.3.doxy \
	chapters/code/hello_pragma2.c \
	chapters/code/hello_pragma.c \
	chapters/code/scal_pragma.cu \
	chapters/code/matmul_pragma.c \
	chapters/code/matmul_pragma2.c \
	chapters/code/cholesky_pragma.c \
	chapters/code/forkmode.c \
	chapters/code/multiformat.c \
	chapters/code/complex.c \
	chapters/code/simgrid.c \
	chapters/code/vector_scal_c.c \
	chapters/code/vector_scal_cpu.c \
	chapters/code/vector_scal_cuda.cu \
	chapters/code/vector_scal_opencl.c \
	chapters/code/vector_scal_opencl_codelet.cl \
	chapters/api/codelet_and_tasks.doxy \
	chapters/api/cuda_extensions.doxy \
	chapters/api/data_interfaces.doxy \
	chapters/api/data_management.doxy \
	chapters/api/data_partition.doxy \
	chapters/api/expert_mode.doxy \
	chapters/api/explicit_dependencies.doxy \
	chapters/api/fft_support.doxy \
	chapters/api/fxt_support.doxy \
	chapters/api/implicit_dependencies.doxy \
	chapters/api/initialization.doxy \
	chapters/api/insert_task.doxy \
	chapters/api/lower_bound.doxy \
	chapters/api/misc_helpers.doxy \
	chapters/api/mpi.doxy \
	chapters/api/multiformat_data_interface.doxy \
	chapters/api/opencl_extensions.doxy \
	chapters/api/parallel_tasks.doxy \
	chapters/api/performance_model.doxy \
	chapters/api/profiling.doxy \
	chapters/api/running_driver.doxy \
	chapters/api/scheduling_contexts.doxy \
	chapters/api/scheduling_policy.doxy \
	chapters/api/standard_memory_library.doxy \
	chapters/api/task_bundles.doxy \
	chapters/api/task_lists.doxy \
	chapters/api/top.doxy \
	chapters/api/versioning.doxy \
	chapters/api/workers.doxy \
	chapters/api/threads.doxy \
	chapters/api/toolbox.doxy \
	chapters/api/sc_hypervisor/sc_hypervisor.doxy \
	chapters/api/sc_hypervisor/sc_hypervisor_usage.doxy

starpu_config.h: $(top_srcdir)/include/starpu_config.h.in
	sed 's/#undef \(.*\)/#define \1 1/' $< > $@

chapters/version.sty: $(chapters)
	for f in $(chapters) ; do \
                if test -f $(top_srcdir)/doc/doxygen/$$f ; then stat --format=%Y $(top_srcdir)/doc/doxygen/$$f ; fi \
        done | sort -r | head -1 > timestamp_sty
	if test -s timestamp_sty ; then \
		LC_ALL=C date --date=@`cat timestamp_sty` +"%d %B %Y" > timestamp_sty_updated ;\
		LC_ALL=C date --date=@`cat timestamp_sty` +"%B %Y" > timestamp_sty_updated_month ;\
	fi
	if test -s timestamp_sty_updated ; then \
		echo "\newcommand{\STARPUUPDATED}{"`cat timestamp_sty_updated`"}" > $(top_srcdir)/doc/doxygen/chapters/version.sty;\
	else \
		echo "\newcommand{\STARPUUPDATED}{unknown date}" > $(top_srcdir)/doc/doxygen/chapters/version.sty;\
	fi
	echo "\newcommand{\STARPUVERSION}{$(VERSION)}" >> $(top_srcdir)/doc/doxygen/chapters/version.sty
	for f in timestamp_sty timestamp_sty_updated timestamp_sty_updated_month ; do \
		if test -f $$f ; then $(RM) $$f ; fi ;\
	done

chapters/version.html: $(chapters)
	for f in $(chapters) ; do \
                if test -f $(top_srcdir)/doc/doxygen/$$f ; then stat --format=%Y $(top_srcdir)/doc/doxygen/$$f ; fi \
        done | sort -r | head -1 > timestamp_html
	if test -s timestamp_html ; then \
		LC_ALL=C date --date=@`cat timestamp_html` +"%d %B %Y" > timestamp_html_updated ;\
		LC_ALL=C date --date=@`cat timestamp_html` +"%B %Y" > timestamp_html_updated_month ;\
	fi
	echo "This manual documents the usage of StarPU version $(VERSION)." > $(top_srcdir)/doc/doxygen/chapters/version.html
	if test -s timestamp_html_updated ; then \
		echo "Its contents was last updated on "`cat timestamp_html_updated`"." >> $(top_srcdir)/doc/doxygen/chapters/version.html;\
	else \
		echo "Its contents was last updated on <em>unknown_date</em>." >> $(top_srcdir)/doc/doxygen/chapters/version.html;\
	fi
	for f in timestamp_html timestamp_html_updated timestamp_html_updated_month ; do \
		if test -f $$f ; then $(RM) $$f ; fi ;\
	done

EXTRA_DIST	= 		\
	$(chapters) 		\
	chapters/version.sty	\
	chapters/version.html	\
	chapters/tasks_size_overhead.png	\
	chapters/tasks_size_overhead.eps	\
	chapters/tasks_size_overhead.pdf	\
	chapters/distrib_data.png	\
	chapters/distrib_data.eps	\
	chapters/distrib_data.pdf	\
	chapters/distrib_data_histo.png	\
	chapters/distrib_data_histo.eps	\
	chapters/distrib_data_histo.pdf	\
	chapters/paje_draw_histogram.eps	\
	chapters/paje_draw_histogram.png	\
	chapters/paje_draw_histogram.pdf	\
	chapters/starpu_chol_model_11_type.png	\
	chapters/starpu_chol_model_11_type.eps	\
	chapters/starpu_chol_model_11_type.pdf	\
	chapters/starpu_non_linear_memset_regression_based.png	\
	chapters/starpu_non_linear_memset_regression_based.eps	\
	chapters/starpu_non_linear_memset_regression_based.pdf	\
	chapters/starpu_non_linear_memset_regression_based_2.png	\
	chapters/starpu_non_linear_memset_regression_based_2.eps	\
	chapters/starpu_non_linear_memset_regression_based_2.pdf	\
	chapters/starpu_starpu_slu_lu_model_11.png	\
	chapters/starpu_starpu_slu_lu_model_11.eps	\
	chapters/starpu_starpu_slu_lu_model_11.pdf	\
	chapters/temanejo.png		\
	doxygen.cfg 		\
	refman.tex					\
	$(DOX_HTML_DIR)

dox_inputs = $(DOX_CONFIG) 				\
	$(chapters) 					\
	starpu_config.h					\
	chapters/version.sty				\
	chapters/version.html				\
	$(top_srcdir)/include/starpu.h			\
	$(top_srcdir)/include/starpu_data_filters.h	\
	$(top_srcdir)/include/starpu_data_interfaces.h	\
	$(top_srcdir)/include/starpu_worker.h		\
	$(top_srcdir)/include/starpu_task.h		\
	$(top_srcdir)/include/starpu_task_bundle.h	\
	$(top_srcdir)/include/starpu_task_list.h	\
	$(top_srcdir)/include/starpu_task_util.h	\
	$(top_srcdir)/include/starpu_data.h		\
	$(top_srcdir)/include/starpu_perfmodel.h	\
	$(top_srcdir)/include/starpu_util.h		\
	$(top_srcdir)/include/starpu_fxt.h		\
	$(top_srcdir)/include/starpu_cuda.h		\
	$(top_srcdir)/include/starpu_opencl.h		\
	$(top_srcdir)/include/starpu_expert.h		\
	$(top_srcdir)/include/starpu_profiling.h	\
	$(top_srcdir)/include/starpu_bound.h		\
	$(top_srcdir)/include/starpu_scheduler.h	\
	$(top_srcdir)/include/starpu_sched_ctx.h	\
	$(top_srcdir)/include/starpu_sched_ctx_hypervisor.h		\
	$(top_srcdir)/include/starpu_top.h		\
	$(top_srcdir)/include/starpu_hash.h		\
	$(top_srcdir)/include/starpu_rand.h		\
	$(top_srcdir)/include/starpu_cublas.h		\
	$(top_srcdir)/include/starpu_driver.h		\
	$(top_srcdir)/include/starpu_stdlib.h		\
	$(top_srcdir)/include/starpu_thread.h		\
	$(top_srcdir)/include/starpu_thread_util.h	\
	$(top_srcdir)/mpi/include/starpu_mpi.h 		\
	$(top_srcdir)/sc_hypervisor/include/sc_hypervisor.h 		\
	$(top_srcdir)/sc_hypervisor/include/sc_hypervisor_config.h 	\
	$(top_srcdir)/sc_hypervisor/include/sc_hypervisor_lp.h		\
	$(top_srcdir)/sc_hypervisor/include/sc_hypervisor_monitoring.h	\
	$(top_srcdir)/sc_hypervisor/include/sc_hypervisor_policy.h

$(DOX_TAG): $(dox_inputs)
	rm -fr $(DOX_HTML_DIR) $(DOX_LATEX_DIR)
	$(DOXYGEN) $(DOX_CONFIG)
	sed -i 's/ModuleDocumentation <\/li>/<a class="el" href="modules.html">Modules<\/a>/' html/index.html
	sed -i 's/FileDocumentation <\/li>/<a class="el" href="files.html">Files<\/a>/' html/index.html
	@$(SED) -i '/\\begin{titlepage}/,$$d' $(DOX_LATEX_DIR)/refman.tex
	@cat $(top_srcdir)/doc/doxygen/refman.tex >> $(DOX_LATEX_DIR)/refman.tex

dist_pdf_DATA = $(DOX_PDF)

$(DOX_PDF): $(DOX_TAG) refman.tex
	cp $(top_srcdir)/doc/doxygen/chapters/version.sty $(DOX_LATEX_DIR)
	cp $(top_srcdir)/doc/doxygen/chapters/*pdf $(DOX_LATEX_DIR)
	cd $(DOX_LATEX_DIR); \
	rm -f *.aux *.toc *.idx *.ind *.ilg *.log *.out; \
	sed -i -e 's/__env__/\\_Environment Variables!/' -e 's/\\-\\_\\-\\-\\_\\-env\\-\\_\\-\\-\\_\\-//' ExecutionConfigurationThroughEnvironmentVariables.tex ;\
	sed -i -e 's/__configure__/\\_Configure Options!/' -e 's/\\-\\_\\-\\-\\_\\-configure\\-\\_\\-\\-\\_\\-//' CompilationConfiguration.tex ;\
	sed -i s'/\\item Module\\-Documentation/\\item \\hyperlink{ModuleDocumentation}{Module Documentation}/' index.tex ;\
	sed -i s'/\\item File\\-Documentation/\\item \\hyperlink{FileDocumentation}{File Documentation}/' index.tex ;\
	$(PDFLATEX) refman.tex; \
	$(MAKEINDEX) refman.idx;\
	$(PDFLATEX) refman.tex; \
	done=0; repeat=5; \
	while test $$done = 0 -a $$repeat -gt 0; do \
           if $(EGREP) 'Rerun (LaTeX|to get cross-references right)' refman.log > /dev/null 2>&1; then \
	       $(PDFLATEX) refman.tex; \
	       repeat=`expr $$repeat - 1`; \
	   else \
	       done=1; \
	   fi; \
	done; \
	mv refman.pdf ../$(DOX_PDF)

CLEANFILES = $(DOX_TAG) starpu_config.h \
    -r \
    $(DOX_HTML_DIR) \
    $(DOX_LATEX_DIR) \
    $(DOX_PDF)

# Rule to update documentation on web server. Should only be used locally.
PUBLISHHOST	?= gforge
update-web: $(DOX_PDF)
	scp -pr starpu.pdf html $(PUBLISHHOST):/home/groups/starpu/htdocs/doc

showcheck:
	-cat /dev/null
