# plot.gp
# Grouped bar charts (GRID vs CHUNK) with confidence-interval error bars,
# x-axis = percentage of writes, one PNG per performance-counter variable.
#
# Usage:  gnuplot -c plot_vars.gnu results.csv
# (or just: gnuplot plot_vars.gnu   if the CSV is named results.csv
#  in the same directory)

datafile = ARG1 eq '' ? 'results.csv' : ARG1

set datafile separator ","

# ---- general look ----
set style data histograms
set style histogram clustered gap 1 errorbars linewidth 1.5
set style fill solid 0.85 border -1
set boxwidth 0.9
set grid ytics lc rgb "#cccccc" lt 0
set xtics rotate by -45
set key top right box

# The CSV layout (1 header line + 14 GRID rows + 14 CHUNK rows, same
# write-percentage order in both blocks) lets us select each block with
# `every ::first::last` (0-based RECORD index as seen by gnuplot, which
# counts the header line itself as record 0 - so GRID data is records
# 1-14 and CHUNK data is records 15-28, not 0-13 / 14-27).
# `every` needs a literal range, so we use macros (set macros / @name)
# rather than plain string variables.
set macros
grid_range  = '::1::14'
chunk_range = '::15::28'

# column numbers in results.csv
col_writes = 6
col_ops    = 5

# variable name -> (value column, ci column)
array vars[4]
vars[1] = 'resource_stalls.sb:u'
vars[2] = 'ld_blocks.store_forward:u'
vars[3] = 'ld_blocks_partial.address_alias:u'
vars[4] = 'ld_blocks.no_sr:u'

array valcol[4]
valcol[1] = 8
valcol[2] = 10
valcol[3] = 12
valcol[4] = 14

array cicol[4]
cicol[1] = 9
cicol[2] = 11
cicol[3] = 13
cicol[4] = 15

set terminal pngcairo size 1000,650 enhanced font "Helvetica,12"

do for [i=1:4] {
    vname = vars[i]
    vc = valcol[i]
    cc = cicol[i]

    safe = vname
    safe = substr(safe, 1, strlen(safe))
    # sanitize filename (":" and "." and "/" -> "_")
    do for [k=1:strlen(safe)] {
        c = safe[k:k]
        if (c eq ':' || c eq '.' || c eq '/') { safe = safe[1:k-1] . '_' . safe[k+1:strlen(safe)] }
    }

    set output sprintf('plots/%s.png', safe)
    set title vname noenhanced
    set xlabel 'Percentage of writes (%)'
    set ylabel vname noenhanced
    set key outside top right nobox

    plot datafile every @grid_range  using vc:cc:xtic(sprintf('%.0f%%', 100.0*column(col_writes)/column(col_ops))) \
             title 'GRID'  lc rgb '#3594cc', \
         datafile every @chunk_range using vc:cc \
             title 'CHUNK' lc rgb '#ea801c'
}

unset output
