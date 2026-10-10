// Command closure-extract emits a deterministic, symbol-rooted subset of
// generated Go namespace files. Cross-file type and value dependencies are
// copied into that subset. It is an ABI declaration experiment, not a
// replacement for reviewing generated source and SDK metadata.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"go/ast"
	"go/format"
	"go/parser"
	"go/token"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

type symbolEntry struct {
	name     string
	spec     ast.Spec
	decl     *ast.GenDecl
	file     *parsedFile
	isType   bool
	dupPaths []string
}

type parsedFile struct {
	path    string
	file    *ast.File
	rank    int
	imports map[string]string
}

type manifest struct {
	SourceSHA256    string           `json:"source_sha256"`
	OutputSHA256    string           `json:"output_sha256"`
	Package         string           `json:"package"`
	Roots           []string         `json:"roots"`
	Retained        []string         `json:"retained_symbols"`
	MissingRoots    []string         `json:"missing_roots,omitempty"`
	Imports         []string         `json:"imports"`
	Unresolved      []string         `json:"unresolved_type_refs,omitempty"`
	ExternalRefs    []string         `json:"resolved_external_type_refs,omitempty"`
	ExternalSources []externalSource `json:"external_type_sources,omitempty"`
	Included        []includedDecl   `json:"included_declarations"`
}

type externalSource struct {
	Name string `json:"name"`
	Path string `json:"path"`
}

type includedDecl struct {
	Name string `json:"name"`
	Kind string `json:"kind"`
	Path string `json:"path"`
}

type extractOptions struct {
	indexDir string
	pkgName  string
}

type rankedDecl struct {
	rank int
	pos  token.Pos
	decl ast.Decl
}

type importUse struct {
	path  string
	alias string
}

func main() {
	in := flag.String("in", "", "generated Go source file")
	out := flag.String("out", "", "output Go source file")
	manifestPath := flag.String("manifest", "", "output JSON manifest")
	rootsFlag := flag.String("roots", "", "comma-separated top-level symbol roots")
	indexDir := flag.String("index-dir", "", "directory of sibling generated Go files")
	pkgName := flag.String("package", "", "optional output package name")
	flag.Parse()
	if *in == "" || *out == "" || *manifestPath == "" || strings.TrimSpace(*rootsFlag) == "" {
		fatalf("-in, -out, -manifest and -roots are required")
	}
	opts := extractOptions{indexDir: *indexDir, pkgName: *pkgName}
	if err := runWithOptions(*in, *out, *manifestPath, splitRoots(*rootsFlag), opts); err != nil {
		fatalf("%v", err)
	}
}

func run(input, output, manifestPath string, roots []string) error {
	return runWithOptions(input, output, manifestPath, roots, extractOptions{})
}

func runWithIndex(input, output, manifestPath string, roots []string, indexDir string) error {
	return runWithOptions(input, output, manifestPath, roots, extractOptions{indexDir: indexDir})
}

func runWithOptions(input, output, manifestPath string, roots []string, opts extractOptions) error {
	fset := token.NewFileSet()
	files, err := loadFiles(fset, input, output, manifestPath, opts.indexDir)
	if err != nil {
		return err
	}
	symbols, err := indexSymbols(files)
	if err != nil {
		return err
	}
	selected, missing, err := closeSymbols(symbols, roots)
	if err != nil {
		return err
	}
	if len(missing) != 0 {
		return fmt.Errorf("root symbol(s) not found: %s", strings.Join(missing, ", "))
	}
	importNames := map[string]bool{}
	for name := range selected {
		for local := range symbols[name].file.imports {
			importNames[local] = true
		}
	}
	var unresolved []string
	for name := range selected {
		for _, ref := range typeRefs(symbols[name].spec) {
			if selected[ref] || isPredeclaredType(ref) || importNames[ref] {
				continue
			}
			if strings.Contains(ref, ".") {
				parts := strings.SplitN(ref, ".", 2)
				if importNames[parts[0]] {
					continue
				}
			}
			unresolved = append(unresolved, ref)
		}
	}
	unresolved = uniqueSorted(unresolved)
	if len(unresolved) != 0 {
		return fmt.Errorf("unresolved type reference(s): %s", strings.Join(unresolved, ", "))
	}
	uses := map[string]string{}
	var retained []string
	var included []includedDecl
	emitted := map[ast.Spec]bool{}
	var chunks []rankedDecl
	names := make([]string, 0, len(selected))
	for name := range selected {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		entry := symbols[name]
		if emitted[entry.spec] {
			continue
		}
		emitted[entry.spec] = true
		if err := collectImportUses(entry.spec, entry.file.imports, uses); err != nil {
			return err
		}
		keyGUIDLiterals(entry.spec)
		tok := entry.decl.Tok
		doc := takeSpecDoc(entry.spec, entry.decl)
		specDecl := &ast.GenDecl{Doc: doc, TokPos: entry.spec.Pos(), Tok: tok, Specs: []ast.Spec{entry.spec}}
		chunks = append(chunks, rankedDecl{rank: entry.file.rank, pos: entry.spec.Pos(), decl: specDecl})
		for _, specName := range specNames(entry.spec) {
			kind := "value"
			if _, ok := entry.spec.(*ast.TypeSpec); ok {
				kind = "type"
			}
			retained = append(retained, specName)
			included = append(included, includedDecl{Name: specName, Kind: kind, Path: entry.file.path})
		}
	}
	sort.SliceStable(chunks, func(i, j int) bool {
		if chunks[i].rank != chunks[j].rank {
			return chunks[i].rank < chunks[j].rank
		}
		return chunks[i].pos < chunks[j].pos
	})
	imports, err := emitImports(uses)
	if err != nil {
		return err
	}
	pkgName := files[0].file.Name.Name
	if opts.pkgName != "" {
		pkgName = opts.pkgName
	}
	outFile := &ast.File{Name: ast.NewIdent(pkgName)}
	var importPaths []string
	if len(imports) != 0 {
		specs := make([]ast.Spec, 0, len(imports))
		for _, imp := range imports {
			spec := &ast.ImportSpec{Path: &ast.BasicLit{Kind: token.STRING, Value: strconv.Quote(imp.path)}}
			if imp.alias != "" {
				spec.Name = ast.NewIdent(imp.alias)
			}
			specs = append(specs, spec)
			if imp.alias != "" {
				importPaths = append(importPaths, imp.alias+" "+imp.path)
			} else {
				importPaths = append(importPaths, imp.path)
			}
		}
		outFile.Decls = append(outFile.Decls, &ast.GenDecl{Tok: token.IMPORT, Specs: specs})
	}
	for _, chunk := range chunks {
		outFile.Decls = append(outFile.Decls, chunk.decl)
	}
	var generated strings.Builder
	if err := format.Node(&generated, fset, outFile); err != nil {
		return err
	}
	content := []byte("// Code generated by closure-extract. DO NOT EDIT.\n\n" + generated.String())
	if err := os.WriteFile(output, content, 0644); err != nil {
		return err
	}
	sort.Strings(retained)
	sort.Strings(importPaths)
	sort.Slice(included, func(i, j int) bool {
		if included[i].Name != included[j].Name {
			return included[i].Name < included[j].Name
		}
		if included[i].Kind != included[j].Kind {
			return included[i].Kind < included[j].Kind
		}
		return included[i].Path < included[j].Path
	})
	m := manifest{
		SourceSHA256: hashFile(input),
		OutputSHA256: hashBytes(content),
		Package:      pkgName,
		Roots:        roots,
		Retained:     retained,
		Imports:      importPaths,
		Included:     included,
	}
	encoded, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	encoded = append(encoded, '\n')
	return os.WriteFile(manifestPath, encoded, 0644)
}

func loadFiles(fset *token.FileSet, input, output, manifestPath, indexDir string) ([]*parsedFile, error) {
	primary, err := parser.ParseFile(fset, input, nil, parser.ParseComments|parser.AllErrors)
	if err != nil {
		return nil, err
	}
	files := []*parsedFile{{path: input, file: primary, rank: 0}}
	if indexDir == "" {
		if err := fillImports(files); err != nil {
			return nil, err
		}
		return files, nil
	}
	entries, err := os.ReadDir(indexDir)
	if err != nil {
		return nil, fmt.Errorf("read symbol index directory: %w", err)
	}
	inputAbs, _ := filepath.Abs(input)
	outputAbs, _ := filepath.Abs(output)
	manifestAbs, _ := filepath.Abs(manifestPath)
	var siblings []string
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".go") {
			continue
		}
		path := filepath.Join(indexDir, entry.Name())
		pathAbs, _ := filepath.Abs(path)
		if strings.EqualFold(pathAbs, inputAbs) || strings.EqualFold(pathAbs, outputAbs) || strings.EqualFold(pathAbs, manifestAbs) {
			continue
		}
		siblings = append(siblings, path)
	}
	sort.Strings(siblings)
	for i, path := range siblings {
		parsed, err := parser.ParseFile(fset, path, nil, parser.ParseComments|parser.AllErrors)
		if err != nil {
			return nil, fmt.Errorf("index %s: %w", path, err)
		}
		if parsed.Name.Name != primary.Name.Name {
			return nil, fmt.Errorf("package mismatch: %s declares %s, %s declares %s", input, primary.Name.Name, path, parsed.Name.Name)
		}
		files = append(files, &parsedFile{path: path, file: parsed, rank: i + 1})
	}
	if err := fillImports(files); err != nil {
		return nil, err
	}
	return files, nil
}

func fillImports(files []*parsedFile) error {
	for _, file := range files {
		imports, err := fileImportMap(file.file)
		if err != nil {
			return fmt.Errorf("%s: %w", file.path, err)
		}
		file.imports = imports
	}
	return nil
}

func fileImportMap(file *ast.File) (map[string]string, error) {
	result := map[string]string{}
	for _, imp := range file.Imports {
		path := strings.Trim(imp.Path.Value, `"`)
		local := importBase(path)
		if imp.Name != nil {
			if imp.Name.Name == "." || imp.Name.Name == "_" {
				return nil, fmt.Errorf("unsupported import %s %q", imp.Name.Name, path)
			}
			local = imp.Name.Name
		}
		result[local] = path
	}
	return result, nil
}

func indexSymbols(files []*parsedFile) (map[string]*symbolEntry, error) {
	symbols := map[string]*symbolEntry{}
	for _, file := range files {
		for _, decl := range file.file.Decls {
			group, ok := decl.(*ast.GenDecl)
			if !ok || group.Tok == token.IMPORT {
				continue
			}
			for _, spec := range group.Specs {
				names, isType := specNamesAndKind(spec)
				if names == nil {
					continue
				}
				for _, name := range names {
					previous, exists := symbols[name]
					if !exists {
						symbols[name] = &symbolEntry{name: name, spec: spec, decl: group, file: file, isType: isType}
						continue
					}
					// Record the clash and keep the first declaration. Selection fails
					// closed if this name is actually required, so unused namespace
					// collisions do not block a rooted subset.
					if isType {
						previous.isType = true
					}
					previous.dupPaths = append(previous.dupPaths, file.path)
				}
			}
		}
	}
	return symbols, nil
}

func closeSymbols(symbols map[string]*symbolEntry, roots []string) (map[string]bool, []string, error) {
	selected := map[string]bool{}
	var missing []string
	for _, root := range roots {
		entry, ok := symbols[root]
		if !ok {
			missing = append(missing, root)
			continue
		}
		if err := rejectDuplicate(entry); err != nil {
			return nil, nil, err
		}
	}
	if len(missing) != 0 {
		return nil, missing, nil
	}
	queue := append([]string(nil), roots...)
	for len(queue) > 0 {
		name := queue[0]
		queue = queue[1:]
		if selected[name] {
			continue
		}
		entry, ok := symbols[name]
		if !ok {
			continue
		}
		if err := rejectDuplicate(entry); err != nil {
			return nil, nil, err
		}
		selected[name] = true
		for _, ref := range referencedSymbols(entry.spec, symbols) {
			if !selected[ref] {
				queue = append(queue, ref)
			}
		}
	}
	return selected, nil, nil
}

func rejectDuplicate(entry *symbolEntry) error {
	if len(entry.dupPaths) == 0 {
		return nil
	}
	paths := append([]string{entry.file.path}, entry.dupPaths...)
	kind := "value"
	if entry.isType {
		kind = "type"
	}
	return fmt.Errorf("duplicate indexed %s %q in %s", kind, entry.name, strings.Join(paths, ", "))
}

func referencedSymbols(node ast.Node, symbols map[string]*symbolEntry) []string {
	if node == nil {
		return nil
	}
	ignored := map[*ast.Ident]bool{}
	markIgnored(node, ignored)
	seen := map[string]bool{}
	var refs []string
	ast.Inspect(node, func(n ast.Node) bool {
		id, ok := n.(*ast.Ident)
		if !ok || ignored[id] || id.Name == "" || id.Name == "_" {
			return true
		}
		if _, exists := symbols[id.Name]; exists && !seen[id.Name] {
			seen[id.Name] = true
			refs = append(refs, id.Name)
		}
		return true
	})
	sort.Strings(refs)
	return refs
}

func markIgnored(node ast.Node, ignored map[*ast.Ident]bool) {
	ast.Inspect(node, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.SelectorExpr:
			ignored[x.Sel] = true
		case *ast.TypeSpec:
			ignored[x.Name] = true
		case *ast.ValueSpec:
			for _, name := range x.Names {
				ignored[name] = true
			}
		case *ast.Field:
			for _, name := range x.Names {
				ignored[name] = true
			}
		case *ast.FuncDecl:
			if x.Name != nil {
				ignored[x.Name] = true
			}
		case *ast.CompositeLit:
			for _, elt := range x.Elts {
				kv, ok := elt.(*ast.KeyValueExpr)
				if !ok {
					continue
				}
				if id, ok := kv.Key.(*ast.Ident); ok {
					ignored[id] = true
				}
			}
		}
		return true
	})
}

func collectImportUses(node ast.Node, locals map[string]string, uses map[string]string) error {
	var conflict error
	ast.Inspect(node, func(n ast.Node) bool {
		sel, ok := n.(*ast.SelectorExpr)
		if !ok || conflict != nil {
			return conflict == nil
		}
		id, ok := sel.X.(*ast.Ident)
		if !ok {
			return true
		}
		path, ok := locals[id.Name]
		if !ok {
			return true
		}
		if previous, exists := uses[id.Name]; exists && previous != path {
			conflict = fmt.Errorf("import name %s resolves to both %s and %s", id.Name, previous, path)
			return false
		}
		uses[id.Name] = path
		return true
	})
	return conflict
}

func emitImports(uses map[string]string) ([]importUse, error) {
	emitted := make([]importUse, 0, len(uses))
	seenLocal := map[string]string{}
	for local, path := range uses {
		alias := ""
		if local != importBase(path) {
			alias = local
		}
		name := local
		if previous, exists := seenLocal[name]; exists && previous != path {
			return nil, fmt.Errorf("import name %s resolves to both %s and %s", name, previous, path)
		}
		seenLocal[name] = path
		emitted = append(emitted, importUse{path: path, alias: alias})
	}
	sort.Slice(emitted, func(i, j int) bool {
		if emitted[i].path != emitted[j].path {
			return emitted[i].path < emitted[j].path
		}
		return emitted[i].alias < emitted[j].alias
	})
	return emitted, nil
}

func takeSpecDoc(spec ast.Spec, group *ast.GenDecl) *ast.CommentGroup {
	switch s := spec.(type) {
	case *ast.TypeSpec:
		doc := s.Doc
		s.Doc = nil
		if doc != nil {
			return doc
		}
	case *ast.ValueSpec:
		doc := s.Doc
		s.Doc = nil
		if doc != nil {
			return doc
		}
	}
	if group != nil && len(group.Specs) == 1 {
		return group.Doc
	}
	return nil
}

func specNames(spec ast.Spec) []string {
	names, _ := specNamesAndKind(spec)
	return names
}

func specNamesAndKind(spec ast.Spec) ([]string, bool) {
	switch s := spec.(type) {
	case *ast.TypeSpec:
		return []string{s.Name.Name}, true
	case *ast.ValueSpec:
		names := make([]string, 0, len(s.Names))
		for _, name := range s.Names {
			names = append(names, name.Name)
		}
		return names, false
	default:
		return nil, false
	}
}

func keyGUIDLiterals(node ast.Node) {
	ast.Inspect(node, func(n ast.Node) bool {
		lit, ok := n.(*ast.CompositeLit)
		if !ok || !isGUIDType(lit.Type) || len(lit.Elts) != 4 {
			return true
		}
		for _, elt := range lit.Elts {
			if _, keyed := elt.(*ast.KeyValueExpr); keyed {
				return true
			}
		}
		keys := []string{"Data1", "Data2", "Data3", "Data4"}
		for i, elt := range lit.Elts {
			lit.Elts[i] = &ast.KeyValueExpr{Key: ast.NewIdent(keys[i]), Value: elt}
		}
		return true
	})
}

func isGUIDType(expr ast.Expr) bool {
	sel, ok := expr.(*ast.SelectorExpr)
	return ok && sel.Sel != nil && sel.Sel.Name == "GUID"
}

func importBase(path string) string {
	if i := strings.LastIndex(path, "/"); i >= 0 {
		return path[i+1:]
	}
	return path
}

func splitRoots(value string) []string {
	parts := strings.Split(value, ",")
	seen := map[string]bool{}
	result := []string{}
	for _, part := range parts {
		name := strings.TrimSpace(part)
		if name != "" && !seen[name] {
			result = append(result, name)
			seen[name] = true
		}
	}
	sort.Strings(result)
	return result
}

func hashFile(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return ""
	}
	return hex.EncodeToString(h.Sum(nil))
}

func hashBytes(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(2)
}

// typeRefs collects names used in type positions only. It intentionally does
// not treat struct field names or method names as dependencies.
func typeRefs(spec ast.Spec) []string {
	seen := map[string]bool{}
	var visit func(ast.Expr)
	visit = func(expr ast.Expr) {
		switch n := expr.(type) {
		case *ast.Ident:
			seen[n.Name] = true
		case *ast.SelectorExpr:
			if id, ok := n.X.(*ast.Ident); ok {
				seen[id.Name+"."+n.Sel.Name] = true
			} else {
				visit(n.X)
			}
		case *ast.StarExpr:
			visit(n.X)
		case *ast.ArrayType:
			visit(n.Elt)
		case *ast.MapType:
			visit(n.Key)
			visit(n.Value)
		case *ast.ChanType:
			visit(n.Value)
		case *ast.Ellipsis:
			visit(n.Elt)
		case *ast.ParenExpr:
			visit(n.X)
		case *ast.IndexExpr:
			visit(n.X)
			visit(n.Index)
		case *ast.IndexListExpr:
			visit(n.X)
			for _, idx := range n.Indices {
				visit(idx)
			}
		case *ast.FuncType:
			if n.Params != nil {
				for _, field := range n.Params.List {
					visit(field.Type)
				}
			}
			if n.Results != nil {
				for _, field := range n.Results.List {
					visit(field.Type)
				}
			}
		case *ast.StructType:
			for _, field := range n.Fields.List {
				visit(field.Type)
			}
		case *ast.InterfaceType:
			for _, field := range n.Methods.List {
				visit(field.Type)
			}
		}
	}
	switch n := spec.(type) {
	case *ast.TypeSpec:
		visit(n.Type)
	case *ast.ValueSpec:
		if n.Type != nil {
			visit(n.Type)
		}
	}
	refs := make([]string, 0, len(seen))
	for ref := range seen {
		refs = append(refs, ref)
	}
	return refs
}

func isPredeclaredType(name string) bool {
	switch name {
	case "any", "bool", "byte", "comparable", "complex64", "complex128", "error", "float32", "float64", "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr":
		return true
	default:
		return false
	}
}

func uniqueSorted(values []string) []string {
	seen := map[string]bool{}
	result := make([]string, 0, len(values))
	for _, value := range values {
		if !seen[value] {
			seen[value] = true
			result = append(result, value)
		}
	}
	sort.Strings(result)
	return result
}
