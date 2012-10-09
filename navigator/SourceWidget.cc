#include "SourceWidget.h"

#include <QDebug>
#include <QTime>
#include <QMenu>
#include <QPainter>
#include <QLabel>
#include <QTextBlock>
#include <QScrollBar>
#include <cassert>
#include <unordered_map>

#include "CXXSyntaxHighlighter.h"
#include "File.h"
#include "Misc.h"
#include "Project.h"
#include "Ref.h"
#include "ReportRefList.h"
#include "TreeReportWindow.h"
#include "MainWindow.h"

namespace Nav {

const int kTabStopSize = 8;

static inline QSize marginsToSize(const QMargins &margins)
{
    return QSize(margins.left() + margins.right(),
                 margins.top() + margins.bottom());
}

// Measure the size of the line after expanding tab stops.
static int measureLineLength(const QString &data, int start, int size, int tabStopSize)
{
    int pos = 0;
    for (int i = start; i < start + size; ++i) {
        if (data[i].unicode() == '\t')
            pos = (pos + tabStopSize) / tabStopSize * tabStopSize;
        else
            pos++;
    }
    return pos;
}

static inline bool isIdentifierChar(QChar ch)
{
    // XXX: What about C++ destructor references, like ~Foo?  Most likely the
    // index should provide a source range rather than a location, and then the
    // GUI should stop trying to pick out identifiers from the text and rely
    // exclusively on the index data.
    unsigned short u = ch.unicode();
    return u <= 127 && (isalnum(u) || u == '_');
}

static Qt::GlobalColor colorForSyntaxKind(CXXSyntaxHighlighter::Kind kind)
{
    switch (kind) {
    case CXXSyntaxHighlighter::KindComment:
    case CXXSyntaxHighlighter::KindQuoted:
        return Qt::darkGreen;
    case CXXSyntaxHighlighter::KindNumber:
    case CXXSyntaxHighlighter::KindDirective:
        return Qt::darkBlue;
    case CXXSyntaxHighlighter::KindKeyword:
        return Qt::darkYellow;
    default:
        return Qt::black;
    }
}

namespace {

class ColorForRef {
public:
    ColorForRef(Project &project) : m_project(project) {
        addMapping("GlobalVariable", Qt::darkCyan);
        addMapping("Field", Qt::darkRed);
        addMapping("Namespace", Qt::darkMagenta);
        addMapping("Struct", Qt::darkMagenta);
        addMapping("Class", Qt::darkMagenta);
        addMapping("Union", Qt::darkMagenta);
        addMapping("Enum", Qt::darkMagenta);
        addMapping("Typedef", Qt::darkMagenta);
    }

    Qt::GlobalColor color(const Ref &ref) const {
        auto it = m_map.find(m_project.querySymbolType(ref.symbolID()));
        return it == m_map.end() ? Qt::transparent : it->second;
    }

private:
    void addMapping(const char *symbolType, Qt::GlobalColor color) {
        indexdb::ID symbolTypeID = m_project.getSymbolTypeID(symbolType);
        if (symbolTypeID != indexdb::kInvalidID)
            m_map[symbolTypeID] = color;
    }

    Project &m_project;
    std::unordered_map<indexdb::ID, Qt::GlobalColor> m_map;
};

class LineLayout {
public:
    // line is 0-based.
    LineLayout(
            const QFont &font,
            const QMargins &margins,
            File &file,
            int line) :
        m_twc(TextWidthCalculator::getCachedTextWidthCalculator(font)),
        m_fm(font)
    {
        m_lineContent = file.lineContent(line);
        m_lineHeight = effectiveLineSpacing(m_fm);
        m_lineTop = margins.top() + line * m_lineHeight;
        m_lineBaselineY = m_lineTop + m_fm.ascent();
        m_lineLeftMargin = margins.left();
        m_lineStartIndex = file.lineStart(line);
        m_tabStopPx = m_fm.width(QChar(' ')) * kTabStopSize;
        m_charIndex = -1;
        m_charIsSurrogatePair = false;
        m_charLeft = 0;
        m_charWidth = 0;
    }

    bool hasMoreChars()     { return nextCharIndex() < m_lineContent.size(); }

    void advanceChar()
    {
        // Skip over the previous character.
        m_charIndex = nextCharIndex();
        m_charLeft += m_charWidth;
        // Analyze the next character.
        QChar ch = m_lineContent.at(m_charIndex);
        if (!ch.isHighSurrogate()) {
            m_charIsSurrogatePair = false;
            if (ch.unicode() == '\t') {
                m_charText.resize(0);
                m_charWidth =
                        (m_charLeft + m_tabStopPx) /
                        m_tabStopPx * m_tabStopPx - m_charLeft;
            } else {
                m_charText.resize(1);
                m_charText[0] = m_lineContent.at(m_charIndex);
                m_charWidth = m_twc.calculate(m_charText);
            }
        } else {
            // TODO: Test that this surrogate pair code actually works.
            m_charIsSurrogatePair = true;
            m_charText.resize(2);
            m_charText[0] = m_lineContent.at(m_charIndex);
            m_charText[1] = m_lineContent.at(m_charIndex + 1);
            m_charWidth = m_twc.calculate(m_charText);
        }
    }

    int charColumn()        { return m_charIndex; }
    int charFileIndex()     { return m_lineStartIndex + m_charIndex; }
    int charLeft()          { return m_lineLeftMargin + m_charLeft; }
    int charWidth()         { return m_charWidth; }
    int lineTop()           { return m_lineTop; }
    int lineHeight()        { return m_lineHeight; }
    int lineBaselineY()     { return m_lineBaselineY; }
    QString &charText()     { return m_charText; }

private:
    int nextCharIndex()     { return m_charIndex + 1 + m_charIsSurrogatePair; }

private:
    TextWidthCalculator m_twc;
    QFontMetrics m_fm;
    QStringRef m_lineContent;
    int m_lineTop;
    int m_lineHeight;
    int m_lineBaselineY;
    int m_lineLeftMargin;
    int m_lineStartIndex;
    int m_tabStopPx;
    int m_charIndex;
    bool m_charIsSurrogatePair;
    int m_charLeft;
    int m_charWidth;
    QString m_charText;
};

} // anonymous namespace


///////////////////////////////////////////////////////////////////////////////
// SourceWidgetLineArea

QSize SourceWidgetLineArea::sizeHint() const
{
    QFontMetrics fm = fontMetrics();
    return QSize(fm.width('9') * QString::number(m_lineCount).size(),
                 fm.height() * std::max(1, m_lineCount)) +
            marginsToSize(m_margins);
}

void SourceWidgetLineArea::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    QFontMetrics fm = fontMetrics();
    const int ch = effectiveLineSpacing(fm);
    const int cw = fm.width('9');
    const int ca = fm.ascent();
    const int line1 = std::max(0, event->rect().top() / ch - 2);
    const int line2 = std::min(m_lineCount - 1, event->rect().bottom() / ch + 2);
    const int thisWidth = width();

    p.setPen(QColor(Qt::darkGray));
    for (int line = line1; line <= line2; ++line) {
        QString text = QString::number(line + 1);
        p.drawText(thisWidth - (text.size() * cw) - m_margins.right(),
                   ch * line + ca + m_margins.top(),
                   text);
    }
}


///////////////////////////////////////////////////////////////////////////////
// SourceWidgetView

SourceWidgetView::SourceWidgetView(const QMargins &margins, Project &project) :
    m_margins(margins),
    m_project(project),
    m_file(NULL)
{
    setBackgroundRole(QPalette::NoRole);
}

void SourceWidgetView::setFile(File *file)
{
    m_file = file;
    m_maxLineLength = 0;
    m_selection = FileRange();

    if (m_file != NULL) {
        QString content = m_file->content();
        auto syntaxColoringKind = CXXSyntaxHighlighter::highlight(content);

        // Color characters according to the lexed character kind.
        m_syntaxColoring.resize(content.size());
        for (size_t i = 0; i < syntaxColoringKind.size(); ++i) {
            m_syntaxColoring[i] = colorForSyntaxKind(syntaxColoringKind[i]);
        }

        // Color characters according to the index's refs.
        ColorForRef colorForRef(m_project);
        m_project.queryFileRefs(*m_file, [=](const Ref &ref) {
            int offset = this->m_file->lineStart(ref.line() - 1);
            Qt::GlobalColor color = colorForRef.color(ref);
            if (color != Qt::transparent) {
                for (int i = ref.column() - 1; i < ref.endColumn() - 1; ++i)
                    m_syntaxColoring[offset + i] = color;
            }
        });

        // Measure the longest line.
        int maxLength = 0;
        for (int i = 0, lineCount = m_file->lineCount(); i < lineCount; ++i) {
            maxLength = std::max(
                        maxLength,
                        measureLineLength(content,
                                          m_file->lineStart(i),
                                          m_file->lineLength(i),
                                          kTabStopSize));
        }
        m_maxLineLength = maxLength;
    }

    updateGeometry();
    update();
}

void SourceWidgetView::paintEvent(QPaintEvent *event)
{
    if (m_file == NULL)
        return;

    const int lineSpacing = effectiveLineSpacing(fontMetrics());
    QPainter painter(this);

    // Fill in a rectangle for the selected identifier.
    if (!m_selection.isEmpty()) {
        assert(m_selection.start.line == m_selection.end.line);
        QPoint pt1 = locationToPoint(m_selection.start);
        QPoint pt2 = locationToPoint(m_selection.end);
        painter.fillRect(pt1.x(), pt1.y(), (pt2 - pt1).x(), lineSpacing,
                         palette().highlight().color());
    }

    // Paint lines in the clip region.
    const int line1 = std::max(event->rect().y() / lineSpacing - 2, 0);
    const int line2 = std::min(event->rect().bottom() / lineSpacing + 2,
                               m_file->lineCount() - 1);
    if (line1 > line2)
        return;

    for (int line = line1; line <= line2; ++line)
        paintLine(painter, line, event->rect());
}

int SourceWidgetView::lineTop(int line)
{
    return effectiveLineSpacing(fontMetrics()) * line + m_margins.top();
}

// line is 0-based.
void SourceWidgetView::paintLine(
        QPainter &painter,
        int line,
        const QRect &rect)
{
    LineLayout lay(font(), m_margins, *m_file, line);
    QColor currentColor(Qt::black);
    painter.setPen(currentColor);

    while (lay.hasMoreChars()) {
        lay.advanceChar();
        if (lay.charLeft() > rect.right())
            break;
        if (lay.charLeft() + lay.charWidth() <= rect.left())
            continue;
        if (!lay.charText().isEmpty()) {
            FileLocation loc(line, lay.charColumn());
            QColor color(m_syntaxColoring[lay.charFileIndex()]);

            // Override the color for selected text.
            if (loc >= m_selection.start && loc < m_selection.end)
                color = palette().highlightedText().color();

            // Set the painter pen when the color changes.
            if (color != currentColor) {
                painter.setPen(color);
                currentColor = color;
            }

            painter.drawText(
                        lay.charLeft(),
                        lay.lineBaselineY(),
                        lay.charText());
        }
    }
}

FileLocation SourceWidgetView::hitTest(QPoint pixel)
{
    if (m_file == NULL)
        return FileLocation();
    QFontMetrics fm = fontMetrics();
    int line = (pixel.y() - m_margins.top()) / effectiveLineSpacing(fm);
    if (line < 0) {
        return FileLocation(0, 0);
    } else if (line >= m_file->lineCount()) {
        return FileLocation(m_file->lineCount(), 0);
    } else {
        LineLayout lay(font(), m_margins, *m_file, line);
        while (lay.hasMoreChars()) {
            lay.advanceChar();
            if (pixel.x() < lay.charLeft() + lay.charWidth())
                return FileLocation(line, lay.charColumn());
        }
        return FileLocation(line, m_file->lineLength(line));
    }
}

QPoint SourceWidgetView::locationToPoint(FileLocation loc)
{
    if (m_file == NULL || loc.isNull())
        return QPoint(m_margins.left(), m_margins.top());
    if (loc.line >= m_file->lineCount())
        return QPoint(m_margins.left(), lineTop(m_file->lineCount()));
    LineLayout lay(font(), m_margins, *m_file, loc.line);
    while (lay.hasMoreChars()) {
        lay.advanceChar();
        if (lay.charColumn() == loc.column)
            return QPoint(lay.charLeft(), lineTop(loc.line));
    }
    return QPoint(lay.charLeft() + lay.charWidth(), lineTop(loc.line));
}

QSize SourceWidgetView::sizeHint() const
{
    if (m_file == NULL)
        return marginsToSize(m_margins);
    QFontMetrics fontMetrics(font());
    return QSize(m_maxLineLength * fontMetrics.width(' '),
                 m_file->lineCount() * effectiveLineSpacing(fontMetrics)) +
            marginsToSize(m_margins);
}

FileRange SourceWidgetView::findWordAtLocation(const FileLocation &pt)
{
    if (m_file == NULL || !pt.doesPointAtChar(*m_file))
        return FileRange();
    QStringRef content = m_file->lineContent(pt.line);
    if (!isIdentifierChar(content.at(pt.column)))
        return FileRange(pt, FileLocation(pt.line, pt.column + 1));
    int x1 = pt.column;
    while (x1 - 1 >= 0 && isIdentifierChar(content.at(x1 - 1)))
        x1--;
    int x2 = pt.column;
    while (x2 + 1 < content.size() && isIdentifierChar(content.at(x2 + 1)))
        x2++;
    return FileRange(FileLocation(pt.line, x1), FileLocation(pt.line, x2 + 1));
}

FileRange SourceWidgetView::findWordAtPoint(QPoint pt)
{
    if (m_file != NULL) {
        FileLocation loc = hitTest(pt);
        if (loc.doesPointAtChar(*m_file))
            return findWordAtLocation(loc);
    }
    return FileRange();
}

void SourceWidgetView::mousePressEvent(QMouseEvent *event)
{
    m_selection = findWordAtPoint(event->pos());
    update();
}

void SourceWidgetView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_selection != findWordAtPoint(event->pos())) {
        m_selection = FileRange();
        update();
    }
}

void SourceWidgetView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_selection.isEmpty()) {
        FileRange identifierClicked;
        if (m_selection == findWordAtPoint(event->pos()))
            identifierClicked = m_selection;
        m_selection = FileRange();
        update();

        // Delay the event handling as long as possible.  Clicking a symbol is
        // likely to cause a jump to another location, which will change the
        // selection (and perhaps the file being displayed).
        if (!identifierClicked.isEmpty()) {
            // TODO: Is this behavior really ideal in the case that one
            // location maps to multiple symbols?
            QStringList symbols = theProject->querySymbolsAtLocation(
                        m_file,
                        identifierClicked.start.line + 1,
                        identifierClicked.start.column + 1);
            if (symbols.size() == 1) {
                Ref ref = m_project.findSingleDefinitionOfSymbol(symbols[0]);
                theMainWindow->navigateToRef(ref);
            }
        }
    }
}

void SourceWidgetView::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_selection.isEmpty()) {
        if (m_selection != findWordAtPoint(event->pos()))
            m_selection = FileRange();
    }

    if (m_selection.isEmpty()) {
        QMenu *menu = new QMenu();
        bool backEnabled = false;
        bool forwardEnabled = false;
        emit areBackAndForwardEnabled(backEnabled, forwardEnabled);
        menu->addAction(QIcon::fromTheme("go-previous"), "Back",
                        this, SIGNAL(goBack()))->setEnabled(backEnabled);
        menu->addAction(QIcon::fromTheme("go-next"), "Forward",
                        this, SIGNAL(goForward()))->setEnabled(forwardEnabled);
        menu->addAction("Copy File Path", this, SIGNAL(copyFilePath()));
        menu->addAction("Reveal in Sidebar", this, SIGNAL(revealInSideBar()));
        menu->exec(event->globalPos());
        delete menu;
    } else {
        QMenu *menu = new QMenu();
        int line = m_selection.start.line + 1;
        int column = m_selection.start.column + 1;
        QStringList symbols = theProject->querySymbolsAtLocation(m_file, line, column);
        if (symbols.isEmpty()) {
            QAction *action = menu->addAction("No symbols found");
            action->setEnabled(false);
        } else {
            for (const QString &symbol : symbols) {
                QAction *action = menu->addAction(symbol);
                action->setEnabled(false);
                //action->setSeparator(true);
                if (1) {
                    QFont f = action->font();
                    f.setBold(true);
                    action->setFont(f);
                }
                menu->addSeparator();
                action = menu->addAction("Cross-references...", this, SLOT(actionCrossReferences()));
                action->setData(symbol);
                menu->addSeparator();
            }
        }
        menu->exec(event->globalPos());
        delete menu;
    }

    update();
}

void SourceWidgetView::actionCrossReferences()
{
    QAction *action = qobject_cast<QAction*>(sender());
    QString symbol = action->data().toString();
    ReportRefList *r = new ReportRefList(theProject, symbol);
    TreeReportWindow *tw = new TreeReportWindow(r);
    tw->show();
}


///////////////////////////////////////////////////////////////////////////////
// SourceWidget

SourceWidget::SourceWidget(Project &project, QWidget *parent) :
    QScrollArea(parent),
    m_project(project)
{
    setWidgetResizable(false);
    setWidget(new SourceWidgetView(QMargins(4, 4, 4, 4), project));
    setBackgroundRole(QPalette::Base);
    setViewportMargins(30, 0, 0, 0);
    m_lineAreaViewport = new QWidget(this);
    m_lineArea = new SourceWidgetLineArea(QMargins(4, 5, 4, 4), m_lineAreaViewport);
    connect(verticalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(layoutSourceWidget()));

    // Configure the widgets to use a small monospace font.  Force characters
    // to have an integral width for simplicity.
    QFont font;
    font.setFamily("Monospace");
    font.setPointSize(8);
    font.setStyleStrategy(QFont::ForceIntegerMetrics);
    font.setKerning(false);
    widget()->setFont(font);
    m_lineArea->setFont(font);

    connect(&sourceWidgetView(), SIGNAL(goBack()), SIGNAL(goBack()));
    connect(&sourceWidgetView(), SIGNAL(goForward()), SIGNAL(goForward()));
    connect(&sourceWidgetView(),
            SIGNAL(areBackAndForwardEnabled(bool&,bool&)),
            SIGNAL(areBackAndForwardEnabled(bool&,bool&)));
    connect(&sourceWidgetView(), SIGNAL(copyFilePath()), SIGNAL(copyFilePath()));
    connect(&sourceWidgetView(), SIGNAL(revealInSideBar()), SIGNAL(revealInSideBar()));

    layoutSourceWidget();
}

void SourceWidget::setFile(File *file)
{
    if (this->file() != file) {
        verticalScrollBar()->setValue(0);
        sourceWidgetView().setFile(file);
        m_lineArea->setLineCount(file != NULL ? file->lineCount() : 0);
        layoutSourceWidget();

        emit fileChanged(file);
    }
}

SourceWidgetView &SourceWidget::sourceWidgetView()
{
    return *qobject_cast<SourceWidgetView*>(widget());
}

void SourceWidget::layoutSourceWidget(void)
{
    QSize lineAreaSizeHint = m_lineArea->sizeHint();
    m_lineAreaViewport->setGeometry(
                0,
                viewport()->rect().top(),
                lineAreaSizeHint.width(),
                viewport()->rect().height());
    m_lineArea->setGeometry(
                0,
                -verticalScrollBar()->value(),
                lineAreaSizeHint.width(),
                lineAreaSizeHint.height());
    setViewportMargins(lineAreaSizeHint.width(), 0, 0, 0);

    // Hack around a Qt problem.  QScrollArea can be configured to resize
    // the widget automatically (i.e. setWidgetResizable(true)), but this
    // isn't working because the window isn't resized soon enough.  (Maybe
    // Qt is waiting until it returns to the event loop?)  Sometimes we
    // want to scroll to the end of the file, but we can't because that's
    // beyond the end of the the stale widget size Qt is keeping.
    //
    // Basically, Qt has a cached widget size that it computed from the
    // sizeHint.  My code notifies Qt that the sizeHint has changed, then
    // it asks Qt to do something involving the widget size, and Qt *still*
    // uses the cached, invalid size.

    QSize sizeHint = sourceWidgetView().sizeHint();
    sizeHint = sizeHint.expandedTo(viewport()->size());
    sourceWidgetView().resize(sizeHint);
}

void SourceWidget::resizeEvent(QResizeEvent *event)
{
    layoutSourceWidget();
    QScrollArea::resizeEvent(event);
}

void SourceWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Home)
        verticalScrollBar()->setValue(0);
    else if (event->key() == Qt::Key_End)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    else
        QScrollArea::keyPressEvent(event);
}

// Line and column indices are 1-based.
void SourceWidget::selectIdentifier(int line, int column)
{
    SourceWidgetView &w = sourceWidgetView();
    FileRange r = w.findWordAtLocation(FileLocation(line - 1, column - 1));
    w.setSelection(r);
    QPoint wordTopLeft = w.locationToPoint(r.start);
    ensureVisible(wordTopLeft.x(), wordTopLeft.y());
}

QPoint SourceWidget::viewportOrigin()
{
    return QPoint(horizontalScrollBar()->value(),
                  verticalScrollBar()->value());
}

void SourceWidget::setViewportOrigin(const QPoint &pt)
{
    horizontalScrollBar()->setValue(pt.x());
    verticalScrollBar()->setValue(pt.y());
}

} // namespace Nav
