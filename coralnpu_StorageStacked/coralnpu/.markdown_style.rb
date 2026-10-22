all
# Match your 2-space indent standard
rule 'ul-indent', :indent => 2
# Allow ordered list numbers
rule 'ol-prefix', :style => :ordered
# We use soft wrapped paragraphs
exclude_rule 'line-length'
# Allow HTML (common for alignment in hardware docs)
exclude_rule 'no-inline-html'
# Disable broken rules removed from latest release
# https://github.com/markdownlint/markdownlint/issues/472
exclude_rule 'MD055'
exclude_rule 'MD056'
exclude_rule 'MD057'