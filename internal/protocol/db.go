package protocol

import (
	"embed"
	"encoding/xml"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

//go:embed xml/*.xml
var embedded embed.FS

type Arg struct {
	Name   string `xml:"name,attr"`
	Type   string `xml:"type,attr"`
	Iface  string `xml:"interface,attr"`
	Enum   string `xml:"enum,attr"`
	Summary string `xml:"summary,attr"`
}

type Entry struct {
	Name  string `xml:"name,attr"`
	Value string `xml:"value,attr"`
}

type EnumDef struct {
	Name     string `xml:"name,attr"`
	Bitfield bool   `xml:"bitfield,attr"`
	Entries  []Entry `xml:"entry"`
}

type MsgDef struct {
	Name string `xml:"name,attr"`
	Type string
	Args []Arg `xml:"arg"`
}

type Iface struct {
	Name     string `xml:"name,attr"`
	Version  int    `xml:"version,attr"`
	Requests []*MsgDef `xml:"request"`
	Events   []*MsgDef `xml:"event"`
	Enums    []*EnumDef `xml:"enum"`
}

type protocolXML struct {
	XMLName   xml.Name  `xml:"protocol"`
	Name      string    `xml:"name,attr"`
	Ifaces    []Iface   `xml:"interface"`
	Copyright string    `xml:"copyright"`
}

type Enum struct {
	Bitfield bool
	ByName   map[string]Entry
	ByValue  map[string]string
}

type DB struct {
	Ifaces map[string]*Iface
	Enums  map[string]*Enum
	Sources []string
}

func New() *DB {
	return &DB{
		Ifaces: map[string]*Iface{},
		Enums:  map[string]*Enum{},
	}
}

func (db *DB) LoadEmbedded() error {
	entries, err := fs.ReadDir(embedded, "xml")
	if err != nil {
		return err
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	for _, n := range names {
		b, err := fs.ReadFile(embedded, "xml/"+n)
		if err != nil {
			continue
		}
		if err := db.parse(b, "embedded:"+n); err != nil {
			fmt.Fprintf(os.Stderr, "wayland-track: skip embedded %s: %v\n", n, err)
		}
	}
	return nil
}

func (db *DB) LoadDirs(dirs []string) int {
	count := 0
	for _, dir := range dirs {
		if dir == "" {
			continue
		}
		filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			if !strings.HasSuffix(strings.ToLower(d.Name()), ".xml") {
				return nil
			}
			b, err := os.ReadFile(path)
			if err != nil {
				return nil
			}
			if err := db.parse(b, path); err != nil {
				return nil
			}
			count++
			return nil
		})
	}
	return count
}

func (db *DB) parse(b []byte, src string) error {
	var p protocolXML
	if err := xml.Unmarshal(b, &p); err != nil {
		return err
	}
	if p.XMLName.Local != "protocol" || len(p.Ifaces) == 0 {
		return fmt.Errorf("not a wayland protocol xml")
	}
	for i := range p.Ifaces {
		iface := &p.Ifaces[i]
		for _, r := range iface.Requests {
			r.Type = "request"
		}
		for _, e := range iface.Events {
			e.Type = "event"
		}
		db.Ifaces[iface.Name] = iface
		for _, en := range iface.Enums {
			key := iface.Name + "." + en.Name
			e := &Enum{Bitfield: en.Bitfield, ByName: map[string]Entry{}, ByValue: map[string]string{}}
			for _, entry := range en.Entries {
				e.ByName[entry.Name] = entry
				e.ByValue[entry.Value] = entry.Name
			}
			db.Enums[key] = e
		}
	}
	db.Sources = append(db.Sources, src)
	return nil
}

func (db *DB) Request(iface string, opcode int) *MsgDef {
	i := db.Ifaces[iface]
	if i == nil || opcode >= len(i.Requests) {
		return nil
	}
	return i.Requests[opcode]
}

func (db *DB) Event(iface string, opcode int) *MsgDef {
	i := db.Ifaces[iface]
	if i == nil || opcode >= len(i.Events) {
		return nil
	}
	return i.Events[opcode]
}

func (db *DB) EnumByKey(key string) *Enum {
	return db.Enums[key]
}

func (db *DB) IfaceNames() []string {
	names := make([]string, 0, len(db.Ifaces))
	for n := range db.Ifaces {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}
