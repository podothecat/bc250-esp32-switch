"""Build the Korean recipient manual. Requires reportlab and Malgun Gothic."""
from pathlib import Path
import os
import re
from html import escape
from io import BytesIO
from PIL import Image as PILImage, ImageOps
from reportlab.pdfgen import canvas
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak, Image, Flowable
from reportlab.lib.utils import ImageReader
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.colors import HexColor, white
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from manual_pinouts import Pinout

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts'
pdfmetrics.registerFont(TTFont('Malgun', str(FONT_DIR / 'malgun.ttf')))
pdfmetrics.registerFont(TTFont('MalgunBold', str(FONT_DIR / 'malgunbd.ttf')))
pdfmetrics.registerFontFamily('Malgun', normal='Malgun', bold='MalgunBold')
INK = HexColor('#192F40')
TEAL = HexColor('#117E88')
styles = {
    'body': ParagraphStyle('body', fontName='Malgun', fontSize=9.5, leading=14.8,
                           textColor=INK, wordWrap='CJK', spaceAfter=6),
    'title': ParagraphStyle('title', fontName='MalgunBold', fontSize=26, leading=33,
                            textColor=INK, spaceAfter=7),
    'section': ParagraphStyle('section', fontName='MalgunBold', fontSize=19, leading=26,
                              textColor=TEAL, spaceAfter=15),
    'heading': ParagraphStyle('heading', fontName='MalgunBold', fontSize=11.4, leading=17,
                              textColor=TEAL, spaceBefore=8, spaceAfter=5, keepWithNext=True),
    'cell': ParagraphStyle('cell', fontName='Malgun', fontSize=9.1, leading=14,
                           textColor=INK, wordWrap='CJK'),
    'headcell': ParagraphStyle('headcell', fontName='MalgunBold', fontSize=9.1, leading=14,
                               textColor=white, wordWrap='CJK'),
}


def rich(text):
    return re.sub(r'\*\*(.+?)\*\*', r'<b>\1</b>', escape(text))


def photo_gallery(entries, width):
    """Place original photos in a two-column grid, honoring camera orientation."""
    cards = []
    for alt, relative_path in entries:
        path = ROOT / 'docs' / relative_path
        with PILImage.open(path) as original:
            oriented = ImageOps.exif_transpose(original)
            image_width, image_height = oriented.size
            buffer = BytesIO()
            oriented.save(buffer, format='JPEG', quality=95)
        buffer.seek(0)
        max_height = 330 if len(entries) == 2 else 215
        scale = min((width / 2 - 28) / image_width, max_height / image_height)
        photo = Image(buffer, width=image_width * scale, height=image_height * scale)
        photo.hAlign = 'CENTER'
        title, caption = alt.split(' | ', 1)
        cards.append([photo, Spacer(1, 7), Paragraph(rich(title), styles['heading']),
                      Paragraph(rich(caption), styles['cell'])])
    rows = [cards[i:i + 2] for i in range(0, len(cards), 2)]
    gallery = Table(rows, colWidths=[width / 2] * 2, hAlign='LEFT')
    gallery.setStyle(TableStyle([
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('LEFTPADDING', (0, 0), (-1, -1), 10),
        ('RIGHTPADDING', (0, 0), (-1, -1), 10),
        ('TOPPADDING', (0, 0), (-1, -1), 8),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 12),
        ('LINEBELOW', (0, 0), (-1, -1), 0.5, HexColor('#D6E2E8')),
    ]))
    return gallery


class BootPhoto(Flowable):
    """Original front photo with a vector callout identifying the BOOT button."""
    def __init__(self, width):
        super().__init__()
        self.width, self.height = width, 300

    def draw(self):
        c = self.canv
        with PILImage.open(ROOT / 'docs/images/ESP32.jpg') as original:
            photo = ImageOps.exif_transpose(original)
            c.drawImage(ImageReader(photo), 35, 10, width=210, height=280)
        # Position measured on the supplied, EXIF-oriented front photograph.
        bx, by = 35 + 210 * 0.535, 10 + 280 * (1 - 0.808)
        c.setStrokeColor(HexColor('#C72E3C'))
        c.setLineWidth(2.5)
        c.circle(bx, by, 15, stroke=1, fill=0)
        path = c.beginPath()
        path.moveTo(bx + 16, by)
        path.lineTo(266, by)
        path.lineTo(284, 113)
        c.drawPath(path)
        c.setFillColor(HexColor('#C72E3C'))
        c.setFont('MalgunBold', 18)
        c.drawString(291, 114, 'BOOT 버튼')
        c.setFillColor(INK)
        c.setFont('Malgun', 10)
        c.drawString(291, 92, 'USB 단자 오른쪽')
        c.drawString(291, 73, 'BC250이 꺼져 있을 때')
        c.setFont('MalgunBold', 11)
        c.drawString(291, 53, '스위치 LED가 빠르게 점멸할 때까지')
        c.drawString(291, 35, '누른 뒤 놓으세요.')


class HostAddressExample(Flowable):
    """Clip the original portal capture and annotate it with PDF vectors."""
    def __init__(self, width):
        super().__init__()
        self.width, self.height = width, 220

    def draw(self):
        c = self.canv
        scale = 0.31
        left, top, bottom = 50, 655, 1310
        x, y = 4, 12
        c.saveState()
        clip = c.beginPath()
        clip.rect(x, y, 860 * scale, (bottom - top) * scale)
        c.clipPath(clip, stroke=0)
        c.drawImage(str(ROOT / 'docs/images/portal/host.png'),
                    x - left * scale, y - (1914 - bottom) * scale,
                    width=960 * scale, height=1914 * scale)
        c.restoreState()
        bx, by = x + (68 - left) * scale, y + (bottom - 1127) * scale
        c.setStrokeColor(HexColor('#E65A25'))
        c.setLineWidth(2)
        c.roundRect(bx, by, 824 * scale, 82 * scale, 5, stroke=1, fill=0)
        c.line(bx + 824 * scale, by + 13, 293, by + 13)
        c.setFillColor(HexColor('#C64B1D'))
        c.setFont('MalgunBold', 11)
        c.drawString(301, by + 22, '여기에 MAC 주소가 나타납니다')
        c.setFillColor(INK)
        c.setFont('Malgun', 9)
        c.drawString(301, by + 5, 'BC250 Power Setup을 선택하면')
        c.drawString(301, by - 10, '감지된 주소가 자동으로 채워집니다.')
        c.drawString(301, by - 32, '주소 확인 → 이 BC250 저장')


class NumberedCanvas(canvas.Canvas):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.states = []

    def showPage(self):
        self.states.append(dict(self.__dict__))
        self._startPage()

    def save(self):
        count = len(self.states)
        for state in self.states:
            self.__dict__.update(state)
            w, h = A4
            self.setStrokeColor(TEAL)
            self.setLineWidth(1)
            self.line(42, h - 32, w - 42, h - 32)
            self.setFont('Malgun', 7.5)
            self.setFillColor(INK)
            self.drawString(42, h - 25, 'BC250  /  USER GUIDE')
            self.drawRightString(w - 42, h - 25, 'ESP32-WROOM-32')
            self.setStrokeColor(HexColor('#D6E2E8'))
            self.line(42, 34, w - 42, 34)
            self.drawString(42, 21, 'BC250 전원 스위치 · 사용 설명서')
            self.drawRightString(w - 42, 21, f'{self._pageNumber} / {count}')
            super().showPage()
        super().save()


def build():
    output = ROOT / 'docs' / 'BC250-사용설명서.pdf'
    doc = SimpleDocTemplate(str(output), pagesize=A4, rightMargin=42, leftMargin=42,
                            topMargin=49, bottomMargin=46, title='BC250 전원 스위치 사용 설명서',
                            author='BC250 ESP32 Switch', pageCompression=1)
    lines = (ROOT / 'docs' / '사용설명서.md').read_text(encoding='utf-8-sig').splitlines()
    story = []
    idx = 0
    while idx < len(lines):
        line = lines[idx].strip()
        idx += 1
        if not line:
            continue
        if line == '<!-- pagebreak -->':
            story.append(PageBreak())
        elif line == '<!-- boot-photo -->':
            story.append(BootPhoto(doc.width))
        elif line == '<!-- gaming-example -->':
            path = ROOT / 'docs/images/bc250-gaming-bluetooth-example.png'
            with PILImage.open(path) as screenshot:
                w, h = screenshot.size
            shot = Image(str(path), width=360, height=360 * h / w)
            story.extend([shot, Spacer(1, 6)])
        elif line == '<!-- host-address-example -->':
            story.append(HostAddressExample(doc.width))
        elif line.startswith('<!-- portal-screen:'):
            name = line.removeprefix('<!-- portal-screen:').removesuffix('-->').strip()
            path = ROOT / 'docs/images/portal' / (name + '.png')
            with PILImage.open(path) as screenshot:
                w, h = screenshot.size
            max_height = 550 if name == 'controllers' else 590
            scale = min(350 / w, max_height / h)
            shot = Image(str(path), width=w * scale, height=h * scale)
            shot.hAlign = 'CENTER'
            story.extend([shot, Spacer(1, 12)])
        elif line.startswith('<!-- pinout:'):
            kind = line.removeprefix('<!-- pinout:').removesuffix('-->').strip()
            story.append(Pinout(kind, doc.width))
        elif line.startswith('!['):
            entries = []
            while True:
                match = re.fullmatch(r'!\[(.+)\]\((.+)\)', line)
                if not match:
                    raise ValueError(f'Invalid photo reference: {line}')
                entries.append(match.groups())
                if idx >= len(lines) or not lines[idx].strip().startswith('!['):
                    break
                line = lines[idx].strip()
                idx += 1
            story.extend([photo_gallery(entries, doc.width), Spacer(1, 9)])
        elif line.startswith('|'):
            rows = [line]
            while idx < len(lines) and lines[idx].strip().startswith('|'):
                rows.append(lines[idx].strip())
                idx += 1
            data = []
            for row in rows:
                cells = [c.strip() for c in row.strip('|').split('|')]
                if all(re.fullmatch(r'[-: ]+', c) for c in cells):
                    continue
                style = styles['headcell' if not data else 'cell']
                data.append([Paragraph(rich(c), style) for c in cells])
            widths = [174, doc.width - 174] if len(data[0]) == 2 else [157, 219, doc.width - 376]
            table = Table(data, colWidths=widths, hAlign='LEFT', repeatRows=1)
            table.setStyle(TableStyle([
                ('BACKGROUND', (0, 0), (-1, 0), TEAL),
                ('ROWBACKGROUNDS', (0, 1), (-1, -1), [HexColor('#F0F5F7'), white]),
                ('VALIGN', (0, 0), (-1, -1), 'TOP'),
                ('LEFTPADDING', (0, 0), (-1, -1), 8),
                ('RIGHTPADDING', (0, 0), (-1, -1), 8),
                ('TOPPADDING', (0, 0), (-1, -1), 6),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
                ('LINEBELOW', (0, -1), (-1, -1), 0.5, HexColor('#D6E2E8')),
            ]))
            story.extend([table, Spacer(1, 8)])
        elif line.startswith('### '):
            story.append(Paragraph(rich(line[4:]), styles['heading']))
        elif line.startswith('## '):
            story.append(Paragraph(rich(line[3:]), styles['section']))
        elif line.startswith('# '):
            story.append(Paragraph(rich(line[2:]), styles['title']))
        else:
            story.append(Paragraph(rich(line), styles['body']))
    doc.build(story, canvasmaker=NumberedCanvas)
    print(output)


if __name__ == '__main__':
    build()
