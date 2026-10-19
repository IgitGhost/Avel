#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTranslator>

namespace avel::app {

// Owns the UI language: loads Avel's and Qt's translations, applies the matching locale and
// remembers the user's choice. Installing a translator makes Qt send QEvent::LanguageChange to
// every widget, which is how the UI retranslates without a restart.
class LanguageManager : public QObject {
    Q_OBJECT

public:
    struct Language {
        QString code;        // ISO 639-1: "en", "ru", "hy"
        QString nativeName;  // Never translated, so people can always find their own language
    };

    explicit LanguageManager(QObject* parent = nullptr);

    static QList<Language> supportedLanguages();

    // The stored choice; an empty string means "follow the Windows display language".
    QString preference() const { return preference_; }
    // The language in use after resolving the preference.
    QString activeLanguage() const { return active_; }

    void loadSavedPreference();
    void setPreference(const QString& code);

Q_SIGNALS:
    void languageChanged();

private:
    static bool isSupported(const QString& code);
    static QString resolve(const QString& preference);
    void apply(const QString& code);

    QTranslator appTranslator_;
    QTranslator qtTranslator_;
    QString preference_;
    QString active_;
};

}  // namespace avel::app
