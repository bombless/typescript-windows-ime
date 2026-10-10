package main

import (
	"encoding/json"
	"go/ast"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestClosureIncludesTransitiveTypesAndIID(t *testing.T) {
	dir := t.TempDir()
	source := `package fixture
import "syscall"
type IUnknown struct { Vtbl *IUnknownVtbl }
type IUnknownVtbl struct { Query uintptr }
type ThreadMgr struct { IUnknown }
type Processor struct { Manager *ThreadMgr; IID syscall.GUID }
var IID_Processor = syscall.GUID{Data1: 7}
type Unrelated struct { Large [1024]byte }
`
	in := filepath.Join(dir, "source.go")
	if err := os.WriteFile(in, []byte(source), 0600); err != nil {
		t.Fatal(err)
	}
	out1, manifest1 := filepath.Join(dir, "one.go"), filepath.Join(dir, "one.json")
	out2, manifest2 := filepath.Join(dir, "two.go"), filepath.Join(dir, "two.json")
	roots := []string{"Processor", "IID_Processor"}
	if err := run(in, out1, manifest1, roots); err != nil {
		t.Fatal(err)
	}
	if err := run(in, out2, manifest2, roots); err != nil {
		t.Fatal(err)
	}
	a, _ := os.ReadFile(out1)
	b, _ := os.ReadFile(out2)
	if string(a) != string(b) {
		t.Fatal("identical input did not produce byte-identical output")
	}
	for _, want := range []string{"Processor", "ThreadMgr", "IUnknown", "IUnknownVtbl", "IID_Processor"} {
		if !strings.Contains(string(a), want) {
			t.Errorf("closure missing %s", want)
		}
	}
	if strings.Contains(string(a), "Unrelated") {
		t.Fatal("unrelated declaration leaked into output")
	}
	if _, err := parser.ParseFile(token.NewFileSet(), out1, a, parser.AllErrors); err != nil {
		t.Fatalf("output is not valid Go: %v", err)
	}
}

func TestMissingRootFailsClosed(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "source.go")
	if err := os.WriteFile(in, []byte("package fixture\ntype Present struct{}\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := run(in, filepath.Join(dir, "out.go"), filepath.Join(dir, "out.json"), []string{"Missing"}); err == nil {
		t.Fatal("expected missing root to fail")
	}
}

func TestCrossFileCopiesTransitiveDeclarations(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "source.go")
	sibling := filepath.Join(dir, "sibling.go")
	if err := os.WriteFile(in, []byte("package fixture\nimport \"syscall\"\ntype Processor struct { Shared *Shared; ID syscall.GUID }\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sibling, []byte("package fixture\ntype Shared struct { Value uintptr }\nconst Limit = 4\n"), 0600); err != nil {
		t.Fatal(err)
	}
	outDir := filepath.Join(dir, "out")
	if err := os.Mkdir(outDir, 0700); err != nil {
		t.Fatal(err)
	}
	out1, manifest1 := filepath.Join(outDir, "one.go"), filepath.Join(outDir, "one.json")
	out2, manifest2 := filepath.Join(outDir, "two.go"), filepath.Join(outDir, "two.json")
	if err := runWithIndex(in, out1, manifest1, []string{"Processor"}, dir); err != nil {
		t.Fatal(err)
	}
	if err := runWithIndex(in, out2, manifest2, []string{"Processor"}, dir); err != nil {
		t.Fatal(err)
	}
	a, err := os.ReadFile(out1)
	if err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(out2)
	if err != nil {
		t.Fatal(err)
	}
	if string(a) != string(b) {
		t.Fatal("identical input did not produce byte-identical output")
	}
	if !strings.Contains(string(a), "type Shared struct") || strings.Contains(string(a), "Limit") {
		t.Fatalf("expected copied Shared type without unreferenced Limit, got:\n%s", a)
	}
	data, err := os.ReadFile(manifest1)
	if err != nil {
		t.Fatal(err)
	}
	var got manifest
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.ExternalRefs) != 0 || len(got.Unresolved) != 0 {
		t.Fatalf("required types were left external or unresolved: %+v", got)
	}
	found := false
	for _, decl := range got.Included {
		if decl.Name == "Shared" && decl.Kind == "type" && decl.Path == sibling {
			found = true
		}
	}
	if !found {
		t.Fatalf("manifest missing Shared provenance from %s: %+v", sibling, got.Included)
	}
	typeCheckOutput(t, out1)
}

func TestCrossFileCycleAndImportAlias(t *testing.T) {
	dir := t.TempDir()
	primary := filepath.Join(dir, "a.go")
	sibling := filepath.Join(dir, "b.go")
	if err := os.WriteFile(primary, []byte("package fixture\nimport \"syscall\"\ntype Processor struct { Child *Child; ID syscall.GUID }\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sibling, []byte("package fixture\nimport sys \"syscall\"\ntype Child struct { Owner *Processor; ID sys.GUID }\n"), 0600); err != nil {
		t.Fatal(err)
	}
	outDir := filepath.Join(dir, "out")
	if err := os.Mkdir(outDir, 0700); err != nil {
		t.Fatal(err)
	}
	out1, manifest1 := filepath.Join(outDir, "one.go"), filepath.Join(outDir, "one.json")
	out2, manifest2 := filepath.Join(outDir, "two.go"), filepath.Join(outDir, "two.json")
	if err := runWithIndex(primary, out1, manifest1, []string{"Processor"}, dir); err != nil {
		t.Fatal(err)
	}
	if err := runWithIndex(primary, out2, manifest2, []string{"Processor"}, dir); err != nil {
		t.Fatal(err)
	}
	a, _ := os.ReadFile(out1)
	b, _ := os.ReadFile(out2)
	if string(a) != string(b) {
		t.Fatal("cycle extraction was not byte-identical")
	}
	text := string(a)
	for _, want := range []string{"type Processor struct", "type Child struct", "\"syscall\"", "sys \"syscall\""} {
		if !strings.Contains(text, want) {
			t.Fatalf("output missing %q:\n%s", want, text)
		}
	}
	ma, _ := os.ReadFile(manifest1)
	mb, _ := os.ReadFile(manifest2)
	if string(ma) != string(mb) {
		t.Fatal("cycle manifests were not byte-identical")
	}
	var got manifest
	if err := json.Unmarshal(ma, &got); err != nil {
		t.Fatal(err)
	}
	paths := map[string]string{}
	for _, decl := range got.Included {
		paths[decl.Name] = decl.Path
	}
	if paths["Processor"] != primary || paths["Child"] != sibling {
		t.Fatalf("provenance paths = %#v", paths)
	}
	typeCheckOutput(t, out1)
}

func TestCrossFileValueDependencyIsCopied(t *testing.T) {
	dir := t.TempDir()
	primary := filepath.Join(dir, "source.go")
	sibling := filepath.Join(dir, "values.go")
	if err := os.WriteFile(primary, []byte("package fixture\ntype Holder struct { Kind Kind }\nvar Default = Limit\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sibling, []byte("package fixture\ntype Kind int\nconst Limit = 4\nconst Unused = 9\n"), 0600); err != nil {
		t.Fatal(err)
	}
	outDir := filepath.Join(dir, "out")
	if err := os.Mkdir(outDir, 0700); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(outDir, "out.go")
	manifestPath := filepath.Join(outDir, "out.json")
	if err := runWithIndex(primary, out, manifestPath, []string{"Holder", "Default"}, dir); err != nil {
		t.Fatal(err)
	}
	text, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(text), "type Kind") || !strings.Contains(string(text), "Limit") || strings.Contains(string(text), "Unused") {
		t.Fatalf("value closure was wrong:\n%s", text)
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatal(err)
	}
	var got manifest
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, decl := range got.Included {
		if decl.Name == "Limit" && decl.Kind == "value" && decl.Path == sibling {
			found = true
		}
	}
	if !found {
		t.Fatalf("Limit provenance missing: %+v", got.Included)
	}
	typeCheckOutput(t, out)
}

func TestMissingCrossFileTypeFailsClosed(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "source.go")
	if err := os.WriteFile(in, []byte("package fixture\ntype Processor struct { Missing *Ghost }\n"), 0600); err != nil {
		t.Fatal(err)
	}
	outDir := filepath.Join(dir, "out")
	if err := os.Mkdir(outDir, 0700); err != nil {
		t.Fatal(err)
	}
	err := runWithIndex(in, filepath.Join(outDir, "out.go"), filepath.Join(outDir, "out.json"), []string{"Processor"}, dir)
	if err == nil || !strings.Contains(err.Error(), "Ghost") {
		t.Fatalf("expected unresolved Ghost to fail closed, got %v", err)
	}
}

func typeCheckOutput(t *testing.T, path string) {
	t.Helper()
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, path, nil, parser.AllErrors)
	if err != nil {
		t.Fatal(err)
	}
	conf := types.Config{Importer: importer.Default()}
	if _, err := conf.Check(file.Name.Name, fset, []*ast.File{file}, nil); err != nil {
		t.Fatalf("extracted output does not type-check: %v", err)
	}
}

func TestCrossFileIndexRejectsDuplicateTypeNames(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "source.go")
	first := filepath.Join(dir, "first.go")
	second := filepath.Join(dir, "second.go")
	for path, source := range map[string]string{
		in:     "package fixture\ntype Processor struct { Shared *Shared }\n",
		first:  "package fixture\ntype Shared struct { A uintptr }\n",
		second: "package fixture\ntype Shared struct { B uintptr }\n",
	} {
		if err := os.WriteFile(path, []byte(source), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := runWithIndex(in, filepath.Join(dir, "out.go"), filepath.Join(dir, "out.json"), []string{"Processor"}, dir); err == nil || !strings.Contains(err.Error(), "duplicate indexed type") {
		t.Fatalf("expected duplicate type index to fail closed, got %v", err)
	}
}
