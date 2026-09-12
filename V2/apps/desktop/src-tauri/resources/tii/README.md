# TII-Senderdatenbank

`txdata.tii` stammt unveraendert aus **Qt-DAB** (Jan van Katwijk, Lazy Chair
Computing, GPL v2 oder spaeter; Datei `res/txdata.tii` des Qt-DAB-Quellbaums).
Sie wird von `crates/dab-app/src/tii.rs` gelesen (Rust-Port von
`tii-reader.cpp`): erstes Byte = Shift-Marker, bei `0xAA` wird jedes Byte mit
`0xAA` XOR-verknuepft, sonst der Shift subtrahiert; danach `;`-getrennte
Textzeilen (`id;country;block;ensemblelabel;eid;tii;location;latidec;longidec;
height;ant;pol;frequency;erp;dirdeg`).

Eine neuere Datei kann als `data/txdata.tii` neben die Einstellungen gelegt
werden; sie hat Vorrang vor dieser Ressource.
