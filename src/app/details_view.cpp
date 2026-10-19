#include "details_view.h"

#include <QDate>
#include <QDesktopServices>
#include <QEvent>
#include <QLocale>
#include <QUrl>

#include "core/inventory/program.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

QString escaped(const std::wstring& text) { return toQString(text).toHtmlEscaped(); }

QString code(const std::wstring& text) { return u"<code>"_s + escaped(text) + u"</code>"_s; }

QString link(const QUrl& url, const QString& text) {
    return u"<a href=\""_s + url.toString(QUrl::FullyEncoded).toHtmlEscaped() + u"\">"_s +
           text.toHtmlEscaped() + u"</a>"_s;
}

}  // namespace

DetailsView::DetailsView(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &DetailsView::openLink);
    render();
}

void DetailsView::setProgram(const inventory::Program* program) {
    program_ = program;
    render();
}

void DetailsView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        render();
    }
    QTextBrowser::changeEvent(event);
}

void DetailsView::openLink(const QUrl& url) {
    // Only links this view generated: web pages and local folders.
    if (url.scheme() == u"http"_s || url.scheme() == u"https"_s || url.isLocalFile()) {
        QDesktopServices::openUrl(url);
    }
}

void DetailsView::render() {
    if (program_ == nullptr) {
        setHtml(u"<p style=\"color: gray\">"_s + tr("Select a program to see its details.").toHtmlEscaped() +
                u"</p>"_s);
        return;
    }
    const inventory::Program& p = *program_;

    QString rows;
    auto row = [&rows](const QString& label, const QString& valueHtml) {
        if (!valueHtml.isEmpty()) {
            rows += u"<tr><td style=\"color: gray; padding-right: 12px; white-space: nowrap\">"_s +
                    label.toHtmlEscaped() + u"</td><td>"_s + valueHtml + u"</td></tr>"_s;
        }
    };

    row(tr("Version"), escaped(p.display_version));
    row(tr("Publisher"), escaped(p.publisher));
    if (p.install_date) {
        const QDate date = QDate(1970, 1, 1).addDays(p.install_date->time_since_epoch().count());
        row(tr("Installed"), QLocale().toString(date, QLocale::LongFormat).toHtmlEscaped());
    }
    if (p.estimated_size_bytes > 0) {
        row(tr("Size"), formatSize(p.estimated_size_bytes).toHtmlEscaped());
    }
    row(tr("Installed for"), ProgramText::scopeLabel(p).toHtmlEscaped());
    row(tr("Architecture"), ProgramText::architectureLabel(p.architecture).toHtmlEscaped());
    row(tr("Type"), ProgramText::typeLabel(p).toHtmlEscaped());
    if (!p.install_location.empty()) {
        row(tr("Location"),
            link(QUrl::fromLocalFile(toQString(p.install_location)), toQString(p.install_location)));
    }
    if (!p.about_url.empty()) {
        const QUrl url(toQString(p.about_url));
        row(tr("Website"),
            url.scheme().startsWith(u"http"_s) ? link(url, toQString(p.about_url)) : escaped(p.about_url));
    }
    if (!p.uninstall_string.empty()) {
        row(tr("Uninstall command"), code(p.uninstall_string));
    }
    if (!p.quiet_uninstall_string.empty() && p.quiet_uninstall_string != p.uninstall_string) {
        row(tr("Silent uninstall command"), code(p.quiet_uninstall_string));
    }
    if (!p.modify_path.empty()) {
        row(tr("Modify command"), code(p.modify_path));
    }
    if (p.registry) {
        row(tr("Registry key"), code(p.registry->path));
    }
    if (!p.msi_product_code.empty()) {
        row(tr("Product code"), code(p.msi_product_code));
    }
    if (!p.package_full_name.empty()) {
        row(tr("Package"), code(p.package_full_name));
    }
    if (!p.comments.empty()) {
        row(tr("Comments"), escaped(p.comments));
    }

    QStringList notes;
    if (p.no_remove) {
        notes << tr("The installer doesn't allow uninstalling this program.");
    }
    if (p.system_component) {
        notes << tr("Marked as a system component: hidden from Settings.");
    }
    if (p.is_update) {
        notes << tr("An update to another program.");
    }
    if (p.is_framework) {
        notes << tr("A framework used by other apps.");
    }
    if (p.msi_product_unregistered) {
        notes << tr(
            "Windows Installer doesn't list this product for this computer or the current user. It may "
            "belong to another user or be left over from an incomplete removal.");
    }
    if (p.source == inventory::Source::Msi) {
        notes << tr("Registered with Windows Installer only, without an entry in Settings.");
    }

    QString notesHtml;
    for (const auto& note : notes) {
        notesHtml += u"<p style=\"color: #b36b00\">"_s + note.toHtmlEscaped() + u"</p>"_s;
    }

    const QString name = p.display_name.empty() ? tr("(no name)") : toQString(p.display_name);
    setHtml(u"<h3>"_s + name.toHtmlEscaped() + u"</h3>"_s + notesHtml + u"<table>"_s + rows + u"</table>"_s);
}

}  // namespace avel::app
