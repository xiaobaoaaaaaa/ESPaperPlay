#!/usr/bin/env python3
"""Exercise the actual C engines under ASan/UBSan; no firmware or SD card required.

Usage: python3 tools/reader/test_reader.py [--sample /path/to/book.epub]
"""
import argparse
import json
import os
import random
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import zipfile
import zlib

ROOT=Path(__file__).resolve().parents[2]
HOST=Path('/tmp/espaperplay-reader-host')
SAMPLE=None

def run(kind,path=None,**env):
    result=subprocess.run([str(HOST),kind]+([str(path)] if path else []),capture_output=True,
                          env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1',**env},timeout=40)
    if result.returncode:
        raise AssertionError(f'C engine exited {result.returncode}: {result.stderr.decode(errors="replace")}')
    return [json.loads(line) for line in result.stdout.splitlines()],result.stderr.decode()

def document(body,head=''):
    return f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Book</title>{head}</head><body>{body}</body></html>'

def png(pixel=b'\xff\0\0'):
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+pixel*2+b'\0'+b'\0\xff\0'*2))+chunk(b'IEND',b'')

def epub(path,docs=None,nav=None,ncx=None,extra=None,opf=None,stored=False):
    docs=docs or [('chapter.xhtml',document('<p>Hello 世界</p>'))]
    manifest=''.join(f'<o:item id="d{i}" href="{name}" media-type="application/xhtml+xml"/>' for i,(name,_) in enumerate(docs))
    if nav:manifest+='<o:item id="nav" href="Nav/nav.xhtml" media-type="application/xhtml+xml" properties="nav scripted"/>'
    if ncx:manifest+='<o:item id="ncx" href="Nav/toc.ncx" media-type="application/x-dtbncx+xml"/>'
    package=opf or f'<o:package xmlns:o="http://www.idpf.org/2007/opf"><o:metadata><x:title xmlns:x="http://purl.org/dc/elements/1.1/">A &amp; B</x:title></o:metadata><o:manifest>{manifest}</o:manifest><o:spine toc="ncx">'+''.join(f'<o:itemref idref="d{i}"/>' for i in range(len(docs)))+'</o:spine></o:package>'
    with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED) as z:
        z.writestr('mimetype','application/epub+zip')
        z.writestr('META-INF/container.xml','<c:container xmlns:c="urn:oasis:names:tc:opendocument:xmlns:container"><c:rootfiles><c:rootfile full-path = "OPS/package.opf" media-type="application/oebps-package+xml"/></c:rootfiles></c:container>')
        z.writestr('OPS/package.opf',package)
        for name,data in docs:z.writestr('OPS/'+name,data)
        if nav:z.writestr('OPS/Nav/nav.xhtml',nav)
        if ncx:z.writestr('OPS/Nav/toc.ncx',ncx)
        for name,data in (extra or {}).items():z.writestr('OPS/'+name,data)
    return path

class ReaderTests(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def book(self,**kwargs):return epub(self.dir/'book.epub',**kwargs)
    def txt(self,raw):p=self.dir/'book.txt';p.write_bytes(raw);rows,_=run('txt',p);self.assertEqual(rows[0]['open'],0);self.assertFalse(any('error' in r for r in rows));return rows
    def parse(self,**kwargs):rows,_=run('epub',self.book(**kwargs));self.assertEqual(rows[0]['open'],0);self.assertFalse(any('error' in r for r in rows));return rows[1:]
    def test_xml_uri_core(self):self.assertEqual(run('core')[0],[{'core':True}])
    def test_displayed_image_survives_background_chapter_load(self):
        p=self.book(docs=[('a.xhtml',document('<img src="a.png"/>')),('b.xhtml',document('<img src="b.png"/>'))],extra={'a.png':png(),'b.png':png(b'\0\0\xff')})
        self.assertEqual(run('image-lifetime',p)[0],[{'image_lifetime':True}])
    def test_txt_encodings(self):
        text='开篇𠀀🙂\r\n第二行\r三\n\n终章'
        for enc,bom in [('utf-8',b''),('utf-8',b'\xef\xbb\xbf'),('utf-16le',b'\xff\xfe'),('utf-16be',b'\xfe\xff'),('utf-32le',b'\xff\xfe\0\0'),('utf-32be',b'\0\0\xfe\xff'),('gb18030',b'')]:
            with self.subTest(encoding=enc,bom=bom):
                rows=self.txt(bom+text.encode(enc));self.assertEqual(''.join(r['text'] for r in rows[1:]),text.replace('\r\n','\n').replace('\r','\n'))
    def test_txt_empty_and_bom_only(self):
        for data in [b'',b'\xef\xbb\xbf',b'\xff\xfe']:
            rows=self.txt(data);self.assertEqual(rows[0]['count'],1);self.assertEqual(rows[1]['text'],'')
    def test_txt_large_and_long_line(self):
        text=('长行𠀀'*30000+'\r\n')*3+'结尾'
        rows=self.txt(text.encode());self.assertGreater(rows[0]['count'],1);self.assertEqual(''.join(r['text'] for r in rows[1:]),text.replace('\r\n','\n'));self.assertTrue(all(len(r['text'].encode())<=65540 for r in rows[1:]))
    def test_txt_dense_lines(self):
        text='\n'*20000+'end';rows=self.txt(text.encode());self.assertEqual(''.join(r['text'] for r in rows[1:]),text);self.assertTrue(all(len(r['blocks'])<=4097 for r in rows[1:]))
    def test_txt_crlf_segment_boundary(self):
        text='a'*49151+'\r\nnext';rows=self.txt(text.encode());self.assertEqual(''.join(r['text'] for r in rows[1:]),text.replace('\r\n','\n'))
    def test_txt_surrogate_and_nul_replacement(self):
        rows=self.txt(b'\xff\xfe'+b'\x00\xd8A\x00\x00\x00');self.assertEqual(rows[1]['text'],'�A�')
    def test_txt_bom_invalid_utf8(self):self.assertEqual(self.txt(b'\xef\xbb\xbf\xffA')[1]['text'],'�A')
    def test_txt_gb_after_ascii_prefix(self):
        text='ASCII '*20000+'汉字𠀀';rows=self.txt(text.encode('gb18030'));self.assertEqual(''.join(r['text'] for r in rows[1:]),text)
    def test_txt_truncated_after_open(self):
        p=self.dir/'truncated.txt';p.write_bytes(b'x'*200000)
        self.assertNotEqual(run('truncated-txt',p)[0][0]['error'],0)
    def test_txt_resource_limit(self):
        p=self.dir/'large.txt'
        with p.open('wb') as f:f.truncate(64*1024*1024+1)
        self.assertNotEqual(run('txt',p)[0][0]['open'],0)
    def test_namespace_and_entities(self):
        c=self.parse(docs=[('chapter.xhtml',document('<x:p xmlns:x="urn:x">A &amp; &#x20000; &copy; <![CDATA[<literal>]]></x:p>'))])[0];self.assertEqual(c['text'],'A & 𠀀 © <literal>')
    def test_epub3_nav_priority_relative_and_anchor(self):
        nav='<html xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="landmarks"><a href="../chapter.xhtml">Wrong</a></nav><nav epub:type="toc"><ol><li><a href="../chapter.xhtml#first">First</a><ol><li><a href="../chapter.xhtml#second">Second</a></li></ol></li></ol></nav></body></html>'
        ncx='<ncx><navMap><navPoint><navLabel><text>OLD NCX</text></navLabel><content src="../chapter.xhtml"/></navPoint></navMap></ncx>'
        c=self.parse(docs=[('chapter.xhtml',document('<p>Intro</p><h1 id="first">One</h1><p>A</p><h2 id="second">Two</h2><p>B</p>'))],nav=nav,ncx=ncx)
        self.assertEqual([x['title'] for x in c],['Book','First','Second']);self.assertEqual(''.join(x['text'] for x in c),'IntroOneATwoB')
    def test_ncx_nested_order(self):
        ncx='<ncx><navMap><navPoint><navLabel><text>Parent</text></navLabel><content src="../chapter.xhtml#a"/><navPoint><navLabel><text>Child</text></navLabel><content src="../chapter.xhtml#b"/></navPoint></navPoint></navMap></ncx>'
        c=self.parse(ncx=ncx,docs=[('chapter.xhtml',document('<p id="a">A</p><p id="b">B</p>'))]);self.assertEqual([x['title'] for x in c][-2:],['Parent','Child']);self.assertEqual(''.join(x['text'] for x in c),'AB')
    def test_unlisted_spine_content_preserved(self):
        ncx='<ncx><navMap><navPoint><navLabel><text>Start</text></navLabel><content src="../a.xhtml"/></navPoint><navPoint><navLabel><text>End</text></navLabel><content src="../c.xhtml"/></navPoint></navMap></ncx>'
        c=self.parse(docs=[('a.xhtml',document('<p>A</p>')),('b.xhtml',document('<p>B</p>')),('c.xhtml',document('<p>C</p>'))],ncx=ncx);self.assertEqual([x['text'] for x in c],['AB','C'])
    def test_anchor_inside_paragraph(self):
        nav='<nav xmlns:epub="urn:epub" epub:type="toc"><a href="../chapter.xhtml#middle">Middle</a></nav>'
        c=self.parse(docs=[('chapter.xhtml',document('<p>Before<span id="middle">Inside</span>After</p>'))],nav=nav);self.assertEqual([x['text'] for x in c],['Before','InsideAfter'])
    def test_styles_cascade_all_sheets_inline_and_import(self):
        head='<link rel="stylesheet" href="a.css"/><link rel="stylesheet" href="b.css"/><style>.box p {font-style:italic} #special {text-align:right !important}</style>'
        c=self.parse(docs=[('chapter.xhtml',document('<div class="box"><p class="normal" id="special" style="text-align:center;font-weight:normal">Styled</p><p>Next</p><p class="hidden">NOPE</p></div>',head))],extra={'a.css':'@import "base.css"; p{font-weight:bold;text-indent:2em}', 'base.css':'.hidden{display:none}', 'b.css':'.normal{font-weight:normal;text-indent:0}'})[0]
        self.assertNotIn('NOPE',c['text']);self.assertEqual(c['blocks'][0][2]&0x8b,0x82);self.assertTrue(c['blocks'][1][2]&1);self.assertTrue(c['blocks'][1][2]&256)
    def test_styles_do_not_leak_documents(self):
        c=self.parse(docs=[('a.xhtml',document('<p>A</p>','<style>p{font-weight:bold}</style>')),('b.xhtml',document('<p>B</p>'))]);self.assertTrue(c[0]['blocks'][0][2]&1);self.assertFalse(c[1]['blocks'][0][2]&1)
    def test_structure_ruby_table_lists_pre(self):
        body='<ol start="3"><li>One</li><li><p>Two</p></li></ol><p><ruby>漢<rp>(</rp><rt>かん</rt><rp>)</rp></ruby><br/>End</p><table><tr><td>A</td><td>B</td></tr></table><pre>a  b\n c</pre>'
        text=self.parse(docs=[('chapter.xhtml',document(body))])[0]['text'];self.assertIn('3. One',text);self.assertIn('4. Two',text);self.assertIn('漢（かん）\nEnd',text);self.assertIn('A | B',text);self.assertIn('a  b\n c',text)
    def test_svg_image_wrapper_and_alt(self):
        body='<svg><image xmlns:xlink="urn:x" xlink:href="images/a%23b.png"/></svg><img src="missing.jpg" alt="Missing illustration"/>'
        rows,_=run('epub',self.book(docs=[('chapter.xhtml',document(body))],extra={'images/a#b.png':png()}),READER_IMAGES='1')
        self.assertEqual(rows[0]['open'],0);self.assertEqual(rows[-1],{'image':0,'error':0,'w':2,'h':2});self.assertIn('Missing illustration',rows[1]['text'])
    def test_image_only_chapter(self):
        c=self.parse(docs=[('chapter.xhtml',document('<img src="a.png"/>'))],extra={'a.png':png()})[0];self.assertEqual(c['text'],'');self.assertEqual(c['blocks'][0][3],0)
    def test_utf16_xml(self):
        c=self.parse(docs=[('chapter.xhtml',document('<p>UTF16正文𠀀</p>').encode('utf-16'))])[0];self.assertEqual(c['text'],'UTF16正文𠀀')
    def test_manifest_fallback(self):
        opf='<package><manifest><item id="bad" href="bad.bin" media-type="application/x-unknown" fallback="ok"/><item id="ok" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="bad"/></spine></package>'
        self.assertEqual(self.parse(opf=opf)[0]['text'],'Hello 世界')
    def test_manifest_fallback_cycle_rejected(self):
        opf='<package><manifest><item id="bad" href="bad.bin" media-type="application/x-unknown" fallback="bad"/></manifest><spine><itemref idref="bad"/></spine></package>'
        self.assertNotEqual(run('epub',self.book(opf=opf))[0][0]['open'],0)
    def test_crc_failure_is_reported(self):
        p=self.book(stored=True);raw=p.read_bytes().replace(b'Hello',b'Jello');p.write_bytes(raw);rows,_=run('epub',p);self.assertEqual(rows[0]['open'],0);self.assertNotEqual(rows[1]['error'],0)
    def test_zip_truncation(self):
        p=self.book();p.write_bytes(p.read_bytes()[:-15]);self.assertNotEqual(run('epub',p)[0][0]['open'],0)
    def test_zip_duplicate_rejected(self):
        p=self.book()
        with zipfile.ZipFile(p,'a') as z:z.writestr('OPS/chapter.xhtml','duplicate')
        self.assertNotEqual(run('epub',p)[0][0]['open'],0)
    def test_resource_size_and_depth(self):
        for body in ['x'*(2*1024*1024+1),'<div>'*70+'Deep'+'</div>'*70,'<p>x</p>'*20001]:
            rows,_=run('epub',self.book(docs=[('chapter.xhtml',document(body))]));self.assertNotEqual(rows[1]['error'],0)
    def test_bad_markup_reports_error(self):
        rows,_=run('epub',self.book(docs=[('chapter.xhtml','<html><body><p attr="unterminated>') ]));self.assertNotEqual(rows[1]['error'],0)
    def test_gray_alpha_png_and_corruption(self):
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,4,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\0\0\0\xff'))+chunk(b'IEND',b'')
        p=self.dir/'alpha.png';p.write_bytes(data);result=run('image',p)[0][0];self.assertEqual(result['error'],0);self.assertEqual(result['pixels'],[65535,0])
        p.write_bytes(data[:-12]);self.assertNotEqual(run('image',p)[0][0]['error'],0)
        corrupt=bytearray(data);corrupt[29]^=1;p.write_bytes(corrupt);self.assertNotEqual(run('image',p)[0][0]['error'],0)
    def test_bounded_css_recursion(self):
        css='@media screen {'*100+'p{font-weight:bold}'+'}'*100
        c=self.parse(docs=[('chapter.xhtml',document('<p>Still readable</p>',f'<style>{css}</style>'))])[0];self.assertEqual(c['text'],'Still readable')
    def test_deterministic_zip_mutations(self):
        p=self.book(stored=True);source=p.read_bytes();rng=random.Random(710)
        for _ in range(45):
            mutated=bytearray(source)
            for _ in range(4):mutated[rng.randrange(len(mutated))]^=rng.randrange(1,256)
            p.write_bytes(mutated);run('epub',p)
    def test_cover_probe_and_failure_cleanup(self):
        p=self.book(docs=[('chapter.xhtml',document('<img src="a.png"/>'))],extra={'a.png':png()})
        self.assertEqual(run('cover',p)[0][0],{'error':0,'w':2,'h':2})
        for i in range(1,45):run('cover',p,READER_FAIL_ALLOC=str(i))
        p=self.book();self.assertNotEqual(run('cover',p)[0][0]['error'],0)
    def test_allocation_failure_cleanup(self):
        p=self.book(docs=[('chapter.xhtml',document('<p>Text</p><img src="a.png"/>'))],extra={'a.png':png()})
        for i in range(1,95):
            with self.subTest(allocation=i):run('epub',p,READER_FAIL_ALLOC=str(i))
    def test_real_sample(self):
        if not SAMPLE:self.skipTest("pass --sample to validate a local EPUB")
        begin=time.monotonic();rows,stats=run('epub',SAMPLE,READER_IMAGES='1');self.assertEqual(rows[0]['open'],0);self.assertFalse(any(r.get('error') for r in rows));chapters=[r for r in rows if 'text' in r];self.assertEqual(len(chapters),rows[0]['count']);print(f'\nSample: {len(chapters)} chapters, {sum(len(r["blocks"]) for r in chapters)} blocks, {sum("image" in r for r in rows)} images, {time.monotonic()-begin:.3f}s; {stats.strip()}')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sample',type=Path);args,remaining=parser.parse_known_args();SAMPLE=args.sample
    subprocess.run([str(ROOT/'tools/reader/build_host.sh'),str(HOST)],check=True)
    unittest.main(argv=[__file__,*remaining],verbosity=2)
