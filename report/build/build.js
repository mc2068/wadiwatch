// Builds the report from content.js into:
//   ../NEXUS_Phase1_Report.docx   (editable deliverable)
//   out/preview.html              (same content, printed to PDF by Chrome for page count and review)
// Run: node build.js
const fs = require("fs");
const path = require("path");
const {
  AlignmentType, BorderStyle, Document, Footer, Header, HeadingLevel, ImageRun, LevelFormat,
  Packer, PageBreak, PageNumber, Paragraph, ShadingType, Table, TableCell, TableRow, TextRun,
  VerticalAlign, WidthType,
} = require("docx");
const { meta, blocks } = require("./content");

const FIG_DIR = path.join(__dirname, "..", "figures");
const OUT_DIR = path.join(__dirname, "out");
const NAVY = "1F3A5F", TEAL = "1B9AAA", GREY = "6B7280", LIGHT = "F3F6FA", RULE = "D5DCE5";
const FONT = "Calibri";
const PAGE_W = 11906, PAGE_H = 16838, MARGIN = 1134; // A4, 2 cm margins (DXA)
const CONTENT_W = PAGE_W - 2 * MARGIN;
const CONTENT_PX = Math.round((CONTENT_W / 1440) * 96);

const pngSize = (file) => {
  const b = fs.readFileSync(file);
  return { w: b.readUInt32BE(16), h: b.readUInt32BE(20) };
};
const isTotalRow = (row) => /^(Total|Net result)/.test(row[0]);
const sectionTitles = blocks.filter((b) => b.t === "h1").map((b) => b.text);

// ---------------------------------------------------------------- DOCX
const runs = (text, base = {}) =>
  text.split("**")
    .map((part, i) => (part === "" ? null : new TextRun({ text: part, ...base, bold: base.bold || i % 2 === 1 })))
    .filter(Boolean);

const para = (text, opts = {}, runOpts = {}) =>
  new Paragraph({ spacing: { after: 110, line: 269 }, ...opts, children: runs(text, { font: FONT, size: 22, ...runOpts }) });

function docxTable(b) {
  const cols = b.widths.map((w) => Math.round((CONTENT_W * w) / 100));
  cols[cols.length - 1] += CONTENT_W - cols.reduce((a, c) => a + c, 0);
  const border = { style: BorderStyle.SINGLE, size: 4, color: RULE };
  const borders = { top: border, bottom: border, left: border, right: border };
  const cell = (text, i, { head = false, bold = false } = {}) =>
    new TableCell({
      width: { size: cols[i], type: WidthType.DXA },
      borders,
      verticalAlign: VerticalAlign.CENTER,
      margins: { top: 50, bottom: 50, left: 90, right: 90 },
      shading: head ? { type: ShadingType.CLEAR, fill: NAVY, color: "auto" } : undefined,
      children: [new Paragraph({
        spacing: { after: 0, line: 245 },
        children: runs(text, { font: FONT, size: 19, bold: head || bold, color: head ? "FFFFFF" : undefined }),
      })],
    });
  const rows = [
    new TableRow({ tableHeader: true, cantSplit: true, children: b.head.map((t, i) => cell(t, i, { head: true })) }),
    ...b.rows.map((r) => new TableRow({ cantSplit: true, children: r.map((t, i) => cell(t, i, { bold: isTotalRow(r) })) })),
  ];
  return [
    new Paragraph({ keepNext: true, spacing: { before: 120, after: 70 },
      children: [new TextRun({ text: b.caption, font: FONT, size: 19, bold: true, color: NAVY })] }),
    new Table({ width: { size: CONTENT_W, type: WidthType.DXA }, columnWidths: cols, rows }),
    new Paragraph({ spacing: { after: 110 }, children: [] }),
  ];
}

function docxFigure(b) {
  const file = path.join(FIG_DIR, b.file);
  const { w, h } = pngSize(file);
  const width = Math.round((CONTENT_PX * b.widthPct) / 100);
  return [
    new Paragraph({ alignment: AlignmentType.CENTER, keepNext: true, spacing: { before: 100, after: 50 },
      children: [new ImageRun({ type: "png", data: fs.readFileSync(file),
        transformation: { width, height: Math.round((width * h) / w) },
        altText: { title: b.caption, description: b.caption, name: b.file } })] }),
    new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 150 },
      children: [new TextRun({ text: b.caption, font: FONT, size: 19, italics: true, color: GREY })] }),
  ];
}

function docxBlock(b) {
  switch (b.t) {
    case "h1": return [new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun(b.text)] })];
    case "h2": return [new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun(b.text)] })];
    case "p": return [para(b.text, { alignment: AlignmentType.JUSTIFIED })];
    case "ul": return b.items.map((t) => para(t, { numbering: { reference: "bullets", level: 0 }, spacing: { after: 60, line: 264 } }));
    case "note": return [para(b.text, {
      shading: { type: ShadingType.CLEAR, fill: LIGHT, color: "auto" },
      border: { left: { style: BorderStyle.SINGLE, size: 18, color: TEAL, space: 8 } },
      spacing: { before: 60, after: 140, line: 264 },
    }, { italics: true, size: 20 })];
    case "table": return docxTable(b);
    case "fig": return docxFigure(b);
    case "refs": return b.items.map((t, i) => para(`[${i + 1}]  ${t}`, { indent: { left: 480, hanging: 480 }, spacing: { after: 50, line: 250 } }, { size: 19 }));
    default: throw new Error("unknown block " + b.t);
  }
}

function docxCover() {
  const line = (text, size, o = {}, po = {}) => new Paragraph({ alignment: AlignmentType.LEFT, spacing: { after: 120 }, ...po,
    children: [new TextRun({ text, font: FONT, size, ...o })] });
  const border = { style: BorderStyle.SINGLE, size: 4, color: RULE };
  const none = { style: BorderStyle.NONE, size: 0, color: "FFFFFF" };
  const cols = [Math.round(CONTENT_W * 0.3), CONTENT_W - Math.round(CONTENT_W * 0.3)];
  const facts = new Table({ width: { size: CONTENT_W, type: WidthType.DXA }, columnWidths: cols,
    rows: meta.facts.map(([k, v]) => new TableRow({ children: [k, v].map((t, i) => new TableCell({
      width: { size: cols[i], type: WidthType.DXA },
      borders: { top: border, bottom: border, left: none, right: none },
      margins: { top: 90, bottom: 90, left: 60, right: 60 },
      children: [new Paragraph({ spacing: { after: 0 }, children: [new TextRun({ text: t, font: FONT, size: 21, bold: i === 0, color: i === 0 ? NAVY : undefined })] })],
    })) })) });
  return [
    line(meta.challenge, 21, { color: GREY }, { spacing: { before: 1500, after: 500 } }),
    line(meta.project, 84, { bold: true, color: NAVY }, { spacing: { after: 160 } }),
    line(meta.subtitle, 32, { color: TEAL }, { spacing: { after: 700 },
      border: { bottom: { style: BorderStyle.SINGLE, size: 12, color: TEAL, space: 18 } } }),
    line(meta.doc, 30, { bold: true }, { spacing: { before: 500, after: 60 } }),
    line(meta.date, 22, { color: GREY }, { spacing: { after: 700 } }),
    facts,
    new Paragraph({ children: [new PageBreak()] }),
    new Paragraph({ spacing: { after: 140 }, children: [new TextRun({ text: "Contents", font: FONT, size: 30, bold: true, color: NAVY })] }),
    ...sectionTitles.map((t) => para(t, { spacing: { after: 30, line: 250 }, indent: { left: 200 } }, { size: 20 })),
    new Paragraph({ spacing: { after: 200 }, children: [] }),
  ];
}

async function buildDocx() {
  const doc = new Document({
    creator: meta.project,
    title: `${meta.project}: ${meta.doc}`,
    description: meta.subtitle,
    styles: {
      default: { document: { run: { font: FONT, size: 22 } } },
      paragraphStyles: [
        { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
          run: { font: FONT, size: 30, bold: true, color: NAVY },
          paragraph: { spacing: { before: 300, after: 130 }, keepNext: true, outlineLevel: 0,
            border: { bottom: { style: BorderStyle.SINGLE, size: 6, color: TEAL, space: 3 } } } },
        { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
          run: { font: FONT, size: 24, bold: true, color: TEAL },
          paragraph: { spacing: { before: 190, after: 80 }, keepNext: true, outlineLevel: 1 } },
      ],
    },
    numbering: { config: [{ reference: "bullets", levels: [{ level: 0, format: LevelFormat.BULLET, text: "•",
      alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 400, hanging: 240 } } } }] }] },
    sections: [{
      properties: { page: { size: { width: PAGE_W, height: PAGE_H }, margin: { top: MARGIN, bottom: MARGIN, left: MARGIN, right: MARGIN } }, titlePage: true },
      headers: {
        default: new Header({ children: [new Paragraph({ alignment: AlignmentType.RIGHT,
          children: [new TextRun({ text: `${meta.project}  |  NEXUS ${meta.doc}`, font: FONT, size: 17, color: GREY })] })] }),
        first: new Header({ children: [new Paragraph({ children: [] })] }),
      },
      footers: {
        default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER,
          children: [new TextRun({ children: [PageNumber.CURRENT], font: FONT, size: 18, color: GREY })] })] }),
        first: new Footer({ children: [new Paragraph({ children: [] })] }),
      },
      children: [...docxCover(), ...blocks.flatMap(docxBlock)],
    }],
  });
  const out = path.join(__dirname, "..", "NEXUS_Phase1_Report.docx");
  fs.writeFileSync(out, await Packer.toBuffer(doc));
  return out;
}

// ---------------------------------------------------------------- HTML preview
const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const inline = (s) => esc(s).split("**").map((part, i) => (i % 2 ? `<b>${part}</b>` : part)).join("");

function htmlBlock(b) {
  switch (b.t) {
    case "h1": return `<h1>${inline(b.text)}</h1>`;
    case "h2": return `<h2>${inline(b.text)}</h2>`;
    case "p": return `<p>${inline(b.text)}</p>`;
    case "ul": return `<ul>${b.items.map((t) => `<li>${inline(t)}</li>`).join("")}</ul>`;
    case "note": return `<p class="note">${inline(b.text)}</p>`;
    case "fig": return `<figure><img src="../../figures/${b.file}" style="width:${b.widthPct}%" alt="${esc(b.caption)}"><figcaption>${inline(b.caption)}</figcaption></figure>`;
    case "refs": return `<ol class="refs">${b.items.map((t, i) => `<li><span>[${i + 1}]</span>${inline(t)}</li>`).join("")}</ol>`;
    case "table": return `<div class="tbl"><div class="cap">${inline(b.caption)}</div><table><colgroup>${b.widths.map((w) => `<col style="width:${w}%">`).join("")}</colgroup>` +
      `<thead><tr>${b.head.map((t) => `<th>${inline(t)}</th>`).join("")}</tr></thead><tbody>` +
      b.rows.map((r) => `<tr${isTotalRow(r) ? ' class="total"' : ""}>${r.map((t) => `<td>${inline(t)}</td>`).join("")}</tr>`).join("") + `</tbody></table></div>`;
    default: throw new Error("unknown block " + b.t);
  }
}

function buildHtml() {
  const css = `
@page { size: A4; margin: 20mm; @bottom-center { content: counter(page); font: 9pt Calibri, sans-serif; color: #${GREY}; }
  @top-right { content: "${meta.project}  |  NEXUS ${meta.doc}"; font: 8.5pt Calibri, sans-serif; color: #${GREY}; } }
@page :first { @bottom-center { content: none; } @top-right { content: none; } }
* { box-sizing: border-box; }
body { font: 11pt/1.36 Calibri, "Segoe UI", sans-serif; color: #1f2933; margin: 0; }
h1 { font-size: 15pt; color: #${NAVY}; border-bottom: 1.2px solid #${TEAL}; padding-bottom: 2pt; margin: 15pt 0 6.5pt; break-after: avoid; }
h2 { font-size: 12pt; color: #${TEAL}; margin: 9.5pt 0 4pt; break-after: avoid; }
p { margin: 0 0 5.5pt; text-align: justify; }
ul { margin: 0 0 5.5pt; padding-left: 15pt; } li { margin-bottom: 3pt; }
.note { background: #${LIGHT}; border-left: 3px solid #${TEAL}; padding: 4pt 8pt; font-style: italic; font-size: 10pt; margin: 3pt 0 7pt; }
figure { margin: 5pt 0 7.5pt; text-align: center; break-inside: avoid; } figure img { max-width: 100%; }
figcaption, .cap { font-size: 9.5pt; } figcaption { font-style: italic; color: #${GREY}; margin-top: 2.5pt; }
.tbl { margin: 6pt 0 11pt; } .cap { font-weight: bold; color: #${NAVY}; margin-bottom: 3.5pt; break-after: avoid; }
table { width: 100%; border-collapse: collapse; table-layout: fixed; font-size: 9.5pt; line-height: 1.24; }
th, td { border: 0.6px solid #${RULE}; padding: 2.5pt 4.5pt; text-align: left; vertical-align: middle; }
th { background: #${NAVY}; color: #fff; } tr { break-inside: avoid; } tr.total td { font-weight: bold; }
.refs { list-style: none; padding: 0; margin: 0; font-size: 9.5pt; } .refs li { padding-left: 24pt; text-indent: -24pt; margin-bottom: 2.5pt; overflow-wrap: anywhere; }
.refs li span { display: inline-block; width: 24pt; text-indent: 0; }
.cover { break-after: page; padding-top: 75pt; }
.cover .ch { color: #${GREY}; margin-bottom: 25pt; } .cover .title { font-size: 42pt; font-weight: bold; color: #${NAVY}; line-height: 1.1; }
.cover .sub { font-size: 16pt; color: #${TEAL}; margin: 8pt 0 0; padding-bottom: 14pt; border-bottom: 2px solid #${TEAL}; text-align: left; }
.cover .doc { font-size: 15pt; font-weight: bold; margin-top: 40pt; } .cover .date { color: #${GREY}; margin-bottom: 35pt; }
.cover table { font-size: 10.5pt; } .cover td { border-left: 0; border-right: 0; padding: 4.5pt 3pt; } .cover td:first-child { width: 30%; font-weight: bold; color: #${NAVY}; }
.toc h3 { font-size: 15pt; color: #${NAVY}; margin: 0 0 7pt; } .toc div { font-size: 10pt; margin: 0 0 1.5pt 10pt; } .toc { margin-bottom: 14pt; }`;
  const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>${meta.project}: ${meta.doc}</title><style>${css}</style></head><body>
<section class="cover"><div class="ch">${esc(meta.challenge)}</div><div class="title">${esc(meta.project)}</div><p class="sub">${esc(meta.subtitle)}</p>
<div class="doc">${esc(meta.doc)}</div><div class="date">${esc(meta.date)}</div>
<table>${meta.facts.map(([k, v]) => `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`).join("")}</table></section>
<section class="toc"><h3>Contents</h3>${sectionTitles.map((t) => `<div>${esc(t)}</div>`).join("")}</section>
${blocks.map(htmlBlock).join("\n")}
</body></html>`;
  fs.mkdirSync(OUT_DIR, { recursive: true });
  const out = path.join(OUT_DIR, "preview.html");
  fs.writeFileSync(out, html);
  return out;
}

(async () => {
  const words = JSON.stringify(blocks).replace(/[^A-Za-z0-9' ]+/g, " ").split(/\s+/).length;
  console.log("docx:", await buildDocx());
  console.log("html:", buildHtml());
  console.log("approx words:", words);
})();
