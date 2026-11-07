// Numeric-only extraction. Never copy chip identifiers, boot text or raw serial
// logs into tracked results. Reject incomplete/nonconforming captures.
package main

import (
	"bufio"
	"encoding/csv"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

type row struct {
	run, rep, n, ops                      int
	experiment, dataset, variant          string
	cycles, us, alloc, free, bytes, delta float64
}

func number(s string) float64 {
	x, e := strconv.ParseFloat(s, 64)
	if e != nil {
		panic(e)
	}
	return x
}
func median(a []float64) float64 {
	b := append([]float64{}, a...)
	sort.Float64s(b)
	return b[len(b)/2]
}
func main() {
	if len(os.Args) != 3 {
		panic("usage: analyze ignored-serial.log output-directory")
	}
	input, e := os.Open(os.Args[1])
	if e != nil {
		panic(e)
	}
	defer input.Close()
	scan := bufio.NewScanner(input)
	var rows []row
	done, correct, configuration := false, false, false
	var memory [][]string
	unicode := map[string][]string{}
	datasets := []string{"tiny_plain", "short_plain", "short_form", "short_path", "long_plain", "long_path", "long_sparse", "long_dense", "long_plus", "invalid_late", "binary_nul"}
	unicodeNames := []string{"utf8_ru_raw", "utf8_ru_pct", "utf8_zh_raw", "utf8_zh_pct", "utf8_ja_raw", "utf8_ja_pct", "utf8_emoji_raw", "utf8_emoji_pct"}
	unique := map[string]bool{}
	for scan.Scan() {
		line := scan.Text()
		if strings.HasPrefix(line, "UTF8_CHECK,") {
			u, err := csv.NewReader(strings.NewReader(line)).Read()
			if err != nil || len(u) != 6 {
				panic("malformed UTF8 validation")
			}
			if _, exists := unicode[u[1]]; exists {
				panic("duplicate UTF8 validation")
			}
			if number(u[3]) != 1024 || number(u[5]) != 4 {
				panic("UTF8 output/variant count mismatch")
			}
			want := 1024.0
			if strings.HasSuffix(u[1], "_pct") {
				want = 3072
			}
			if number(u[2]) != want {
				panic("UTF8 input length mismatch")
			}
			unicode[u[1]] = u[1:]
			continue
		}
		if strings.Contains(line, "task_wdt:") || strings.Contains(line, "Guru Meditation") || strings.Contains(line, "ABORT ") {
			panic("device diagnostic failure")
		}
		if strings.HasPrefix(line, "META ") && strings.Contains(line, "cplusplus=201703 ") && strings.Contains(line, "cpu=160000000 ") {
			configuration = true
		}
		if strings.HasPrefix(line, "MEMORY,") {
			m, e := csv.NewReader(strings.NewReader(line)).Read()
			if e != nil || len(m) != 9 {
				panic("malformed memory profile")
			}
			for _, n := range m[3:] {
				number(n)
			}
			if m[4] != m[5] || number(m[8]) != 0 {
				panic("memory profile leak")
			}
			memory = append(memory, m[1:])
			continue
		}
		if strings.HasPrefix(line, "DONE checksum=") {
			done = true
		}
		if strings.HasPrefix(line, "CORRECTNESS ") && strings.Contains(line, "failures=0 ") {
			correct = true
		}
		if !strings.HasPrefix(line, "CSV,") {
			continue
		}
		c, e := csv.NewReader(strings.NewReader(line)).Read()
		if e != nil || len(c) != 14 {
			panic("malformed CSV")
		}
		key := strings.Join(c[1:6], "/")
		if unique[key] {
			panic("duplicate sample " + key)
		}
		unique[key] = true
		r := row{run: int(number(c[1])), experiment: c[2], dataset: c[3], variant: c[4], rep: int(number(c[5])), n: int(number(c[6])), ops: int(number(c[7])), cycles: number(c[8]), us: number(c[9]), alloc: number(c[10]), free: number(c[11]), bytes: number(c[12]), delta: number(c[13])}
		if r.n <= 0 || r.ops <= 0 || r.cycles <= 0 {
			panic("invalid extent/time")
		}
		rows = append(rows, r)
	}
	if e := scan.Err(); e != nil {
		panic(e)
	}
	if len(unicode) != 0 {
		if len(unicode) != 8 {
			panic("incomplete UTF8 validation")
		}
		for i, name := range unicodeNames {
			u, exists := unicode[name]
			if !exists {
				panic("missing UTF8 validation " + name)
			}
			if i%2 == 1 && u[3] != unicode[unicodeNames[i-1]][3] {
				panic("raw/percent UTF8 checksums differ")
			}
		}
		datasets = append(datasets, unicodeNames...)
	}
	wantRows := 5148 + 468*len(unicode)
	wantGroups := 154 + 14*len(unicode)
	if !done || !correct || !configuration || len(memory) != 44+4*len(unicode) || len(rows) != wantRows {
		panic(fmt.Sprintf("incomplete capture done=%v correctness=%v rows=%d want=%d", done, correct, len(rows), wantRows))
	}
	out := os.Args[2]
	if e = os.MkdirAll(out, 0755); e != nil {
		panic(e)
	}
	if len(unicode) > 0 {
		uf, err := os.Create(filepath.Join(out, "unicode-fixtures.csv"))
		if err != nil {
			panic(err)
		}
		uw := csv.NewWriter(uf)
		uw.Write([]string{"dataset", "input_bytes", "output_bytes", "fnv1a_output", "passing_variants"})
		for _, name := range unicodeNames {
			uw.Write(unicode[name])
		}
		uw.Flush()
		if uw.Error() != nil {
			panic(uw.Error())
		}
		uf.Close()
	}
	mf, e := os.Create(filepath.Join(out, "memory.csv"))
	if e != nil {
		panic(e)
	}
	mw := csv.NewWriter(mf)
	mw.Write([]string{"dataset", "variant", "input_bytes", "allocations", "frees", "requested_bytes", "live_heap_cost_bytes", "retained_heap_bytes"})
	for _, m := range memory {
		mw.Write(m)
	}
	mw.Flush()
	if mw.Error() != nil {
		panic(mw.Error())
	}
	mf.Close()
	file, e := os.Create(filepath.Join(out, "measurements.csv"))
	if e != nil {
		panic(e)
	}
	w := csv.NewWriter(file)
	w.Write([]string{"run", "experiment", "dataset", "variant", "rep", "input_bytes", "operations", "cycles", "microseconds", "allocations", "frees", "requested_bytes", "heap_delta"})
	groups := map[string][]row{}
	for _, r := range rows {
		w.Write([]string{strconv.Itoa(r.run), r.experiment, r.dataset, r.variant, strconv.Itoa(r.rep), strconv.Itoa(r.n), strconv.Itoa(r.ops), fmt.Sprint(r.cycles), fmt.Sprint(r.us), fmt.Sprint(r.alloc), fmt.Sprint(r.free), fmt.Sprint(r.bytes), fmt.Sprint(r.delta)})
		key := r.experiment + "/" + r.dataset + "/" + r.variant
		groups[key] = append(groups[key], r)
	}
	if len(groups) != wantGroups {
		panic("unexpected aggregate group count")
	}
	for _, name := range unicodeNames {
		if len(unicode) == 0 {
			break
		}
		for _, experiment := range []string{"warm", "pressure", "owning"} {
			for _, variant := range []string{"direct", "lookup", "from_chars", "spans"} {
				if len(groups[experiment+"/"+name+"/"+variant]) == 0 {
					panic("missing Unicode candidate cell")
				}
			}
		}
	}
	w.Flush()
	if w.Error() != nil {
		panic(w.Error())
	}
	file.Close()
	keys := make([]string, 0, len(groups))
	for k := range groups {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	summary, e := os.Create(filepath.Join(out, "summary.csv"))
	if e != nil {
		panic(e)
	}
	defer summary.Close()
	sw := csv.NewWriter(summary)
	defer sw.Flush()
	sw.Write([]string{"experiment", "dataset", "variant", "samples", "input_bytes", "cycles_op_median", "cycles_op_min", "cycles_op_max", "net_cycles_op", "net_cycles_input_byte", "net_ns_op_at_160MHz", "allocations_op", "requested_bytes_op", "heap_delta_max"})
	md, e := os.Create(filepath.Join(out, "tables.md"))
	if e != nil {
		panic(e)
	}
	defer md.Close()
	fmt.Fprint(md, "# C3 numeric results\n\nGross cycles/op, median [min; max]. Warm/owning: 27 batches per cell, pressure: 45 single calls. Three rounds.\n\n")
	for _, experiment := range []string{"warm", "pressure", "owning"} {
		fmt.Fprintf(md, "## %s\n\n| Dataset | Direct | Lookup | from_chars | Spans |\n|---|---:|---:|---:|---:|\n", experiment)
		for _, dataset := range datasets {
			fmt.Fprintf(md, "| %s ", dataset)
			for _, variant := range []string{"direct", "lookup", "from_chars", "spans"} {
				g := groups[experiment+"/"+dataset+"/"+variant]
				if len(g) == 0 {
					fmt.Fprint(md, "| — ")
					continue
				}
				var x []float64
				for _, r := range g {
					x = append(x, r.cycles/float64(r.ops))
				}
				sort.Float64s(x)
				fmt.Fprintf(md, "| %.1f [%.1f; %.1f] ", median(x), x[0], x[len(x)-1])
			}
			fmt.Fprintln(md, "|")
		}
		fmt.Fprintln(md)
	}
	for _, key := range keys {
		g := groups[key]
		r := g[0]
		want := 27
		if r.experiment == "pressure" {
			want = 45
		}
		if len(g) != want {
			panic("wrong group count " + key)
		}
		var times, allocs, bytes, delta, base []float64
		for _, x := range g {
			times = append(times, x.cycles/float64(x.ops))
			allocs = append(allocs, x.alloc/float64(x.ops))
			bytes = append(bytes, x.bytes/float64(x.ops))
			delta = append(delta, x.delta)
			if x.alloc != x.free {
				panic("unbalanced allocation/free")
			}
		}
		for _, x := range groups[r.experiment+"/"+r.dataset+"/overhead"] {
			base = append(base, x.cycles/float64(x.ops))
		}
		sort.Float64s(times)
		sort.Float64s(delta)
		net := median(times)
		if len(base) > 0 {
			net -= median(base)
		}
		sw.Write([]string{r.experiment, r.dataset, r.variant, strconv.Itoa(len(g)), strconv.Itoa(r.n), fmt.Sprintf("%.3f", median(times)), fmt.Sprintf("%.3f", times[0]), fmt.Sprintf("%.3f", times[len(times)-1]), fmt.Sprintf("%.3f", net), fmt.Sprintf("%.5f", net/float64(r.n)), fmt.Sprintf("%.3f", net*6.25), fmt.Sprintf("%.3f", median(allocs)), fmt.Sprintf("%.3f", median(bytes)), fmt.Sprintf("%.0f", delta[len(delta)-1])})
	}
	fmt.Printf("Validated %d samples, %d groups; correctness PASS, DONE present, balanced allocation/free\n", len(rows), len(groups))
}
