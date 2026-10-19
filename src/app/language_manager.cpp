#include "language_manager.h"

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr auto kSettingsKey = "ui/language"_L1;
constexpr auto kFallbackLanguage = "en"_L1;

QLocale localeFor(const QString& code) {
    if (code == "ru"_L1) {
        return QLocale(QLocale::Russian, QLocale::Russia);
    }
    if (code == "hy"_L1) {
        return QLocale(QLocale::Armenian, QLocale::Armenia);
    }
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

}  // namespace

LanguageManager::LanguageManager(QObject* parent) : QObject(parent) {}

QList<LanguageManager::Language> LanguageManager::supportedLanguages() {
    return {
        {u"en"_s, u"English"_s},
        {u"ru"_s, u"Русский"_s},
        {u"hy"_s, u"Հայերեն"_s},
    };
}

void LanguageManager::loadSavedPreference() {
    const QString saved = QSettings().value(kSettingsKey).toString();
    preference_ = isSupported(saved) ? saved : QString();
    apply(resolve(preference_));
}

void LanguageManager::setPreference(const QString& code) {
    const QString preference = isSupported(code) ? code : QString();
    if (preference == preference_) {
        return;
    }
    preference_ = preference;
    QSettings().setValue(kSettingsKey, preference_);
    apply(resolve(preference_));
}

bool LanguageManager::isSupported(const QString& code) {
    const auto languages = supportedLanguages();
    return std::any_of(languages.begin(), languages.end(),
                       [&](const Language& language) { return language.code == code; });
}

QString LanguageManager::resolve(const QString& preference) {
    if (!preference.isEmpty()) {
        return preference;
    }
    // Follow Windows: take the first display language in the user's list that Avel supports.
    const QStringList tags = QLocale::system().uiLanguages(QLocale::TagSeparator::Dash);
    for (const QString& tag : tags) {
        const QString code = tag.section(u'-', 0, 0).toLower();
        if (isSupported(code)) {
            return code;
        }
    }
    return kFallbackLanguage;
}

void LanguageManager::apply(const QString& code) {
    if (code == active_) {
        return;
    }
    QCoreApplication::removeTranslator(&appTranslator_);
    QCoreApplication::removeTranslator(&qtTranslator_);

    const QLocale locale = localeFor(code);
    QLocale::setDefault(locale);

    // Catalogs are embedded under :/i18n by src/app/CMakeLists.txt. A missing catalog (e.g. Qt
    // has no Armenian one) simply leaves that part in English.
    if (appTranslator_.load(locale, u"avel"_s, u"_"_s, u":/i18n"_s)) {
        QCoreApplication::installTranslator(&appTranslator_);
    }
    if (qtTranslator_.load(locale, u"qtbase"_s, u"_"_s, u":/i18n"_s)) {
        QCoreApplication::installTranslator(&qtTranslator_);
    }

    active_ = code;
    Q_EMIT languageChanged();
}

}  // namespace avel::app
