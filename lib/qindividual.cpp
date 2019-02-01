/*
 * Copyright 2013 Canonical Ltd.
 *
 * This file is part of contact-service-app.
 *
 * contact-service-app is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qindividual.h"
#include "detail-context-parser.h"
#include "gee-utils.h"
#include "update-contact-request.h"
#include "e-source-ubuntu.h"

#include "common/vcard-parser.h"

#include <folks/folks-eds.h>
#include <libebook/libebook.h>

#include <QtCore/QMutexLocker>

#include <QtVersit/QVersitDocument>
#include <QtVersit/QVersitProperty>
#include <QtVersit/QVersitWriter>
#include <QtVersit/QVersitReader>
#include <QtVersit/QVersitContactImporter>
#include <QtVersit/QVersitContactExporter>

#include <QtContacts/QContactName>
#include <QtContacts/QContactDisplayLabel>
#include <QtContacts/QContactBirthday>
#include <QtContacts/QContactNickname>
#include <QtContacts/QContactAvatar>
#include <QtContacts/QContactOrganization>
#include <QtContacts/QContactEmailAddress>
#include <QtContacts/QContactPhoneNumber>
#include <QtContacts/QContactAddress>
#include <QtContacts/QContactOnlineAccount>
#include <QtContacts/QContactUrl>
#include <QtContacts/QContactGuid>
#include <QtContacts/QContactFavorite>
#include <QtContacts/QContactGender>
#include <QtContacts/QContactNote>
#include <QtContacts/QContactExtendedDetail>

#include "config.h"

using namespace QtVersit;
using namespace QtContacts;

#define X_CREATED_AT              "X-CREATED-AT"
#define X_REMOTE_ID               "X-REMOTE-ID"
#define X_GOOGLE_ETAG             "X-GOOGLE-ETAG"
#define X_GROUP_ID                "X-GROUP-ID"
#define X_DELETED_AT              "X-DELETED-AT"
#define X_AVATAR_REV              "X-AVATAR-REV"

namespace
{
static void gValueGeeSetAddStringFieldDetails(GValue *value,
                                              GType g_type,
                                              const char* v_string,
                                              const QtContacts::QContactDetail &detail,
                                              bool ispreferred)
{
    GeeCollection *collection = (GeeCollection*) g_value_get_object(value);
    if(collection == NULL) {
        collection = GEE_COLLECTION(SET_AFD_NEW());
        g_value_take_object(value, collection);
    }

    FolksAbstractFieldDetails *fieldDetails = NULL;

    if(FALSE) {
    } else if(g_type == FOLKS_TYPE_EMAIL_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_email_field_details_new(v_string, NULL));
    } else if(g_type == FOLKS_TYPE_IM_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_im_field_details_new(v_string, NULL));
    } else if(g_type == FOLKS_TYPE_NOTE_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_note_field_details_new(v_string, NULL, NULL));
    } else if(g_type == FOLKS_TYPE_PHONE_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_phone_field_details_new(v_string, NULL));
    } else if(g_type == FOLKS_TYPE_URL_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_url_field_details_new(v_string, NULL));
    } else if(g_type == FOLKS_TYPE_WEB_SERVICE_FIELD_DETAILS) {
        fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS (
                folks_web_service_field_details_new(v_string, NULL));
    }

    if (fieldDetails == NULL) {
        qWarning() << "Invalid fieldDetails type" << g_type;
    } else {
        galera::DetailContextParser::parseContext(fieldDetails, detail, ispreferred);
        gee_collection_add(collection, fieldDetails);

        g_object_unref(fieldDetails);
    }
}

#define PERSONA_DETAILS_INSERT_STRING_FIELD_DETAILS(\
        details, cDetails, key, value, q_type, g_type, member, prefDetail) \
{ \
    if(cDetails.size() > 0) { \
        value = GeeUtils::gValueSliceNew(G_TYPE_OBJECT); \
        Q_FOREACH(const q_type& detail, cDetails) { \
            if(!detail.isEmpty()) { \
                gValueGeeSetAddStringFieldDetails(value, (g_type), \
                        detail.member().toUtf8().data(), \
                        detail, \
                        detail == prefDetail); \
            } \
        } \
        GeeUtils::personaDetailsInsert((details), (key), (value)); \
    } \
}

static QString unaccent(const QString &value)
{
    QString s2 = value.normalized(QString::NormalizationForm_D);
    QString out;

    for (int i=0, j=s2.length(); i<j; i++)
    {
        // strip diacritic marks
        if (s2.at(i).category() != QChar::Mark_NonSpacing &&
            s2.at(i).category() != QChar::Mark_SpacingCombining) {
            out.append(s2.at(i));
        }
    }
    return out;
}

}

namespace galera
{
bool QIndividual::m_autoLink = false;
QStringList QIndividual::m_supportedExtendedDetails;

QIndividual::QIndividual(FolksIndividual *individual, FolksIndividualAggregator *aggregator)
    : m_individual(0),
      m_aggregator(aggregator),
      m_contact(0),
      m_currentUpdate(0),
      m_visible(true)
{
    if (m_supportedExtendedDetails.isEmpty()) {
        m_supportedExtendedDetails << X_CREATED_AT
                                   << X_REMOTE_ID
                                   << X_GOOGLE_ETAG
                                   << X_GROUP_ID
                                   << X_AVATAR_REV;
    }
    setIndividual(individual);
}

void QIndividual::notifyUpdate()
{
    for(int i=0; i < m_listeners.size(); i++) {
        QPair<QObject*, QMetaMethod> listener = m_listeners[i];
        listener.second.invoke(listener.first, Q_ARG(QIndividual*, this));
    }
}

QIndividual::~QIndividual()
{
    if (m_currentUpdate) {
        m_currentUpdate->disconnect(m_updateConnection);
        // this will leave the update object to destroy itself
        // this is necessary because the individual can be destroyed during a update
        // Eg. If the individual get linked
        m_currentUpdate->deatach();
        m_currentUpdate = 0;
    }
    clear();
}

QString QIndividual::id() const
{
    return m_id;
}

QtContacts::QContactDetail QIndividual::getUid() const
{
    QContactGuid uid;
    const char* id = folks_individual_get_id(m_individual);
    Q_ASSERT(id);
    uid.setGuid(QString::fromUtf8(id));

    return uid;
}

QList<QtContacts::QContactDetail> QIndividual::getSyncTargets() const
{
    QList<QContactDetail> details;
    Q_FOREACH(const QString id, m_personas.keys()) {
        QContactSyncTarget target;

        FolksPersona *p = m_personas[id];
        FolksPersonaStore *ps = folks_persona_get_store(p);

        QString displayName = folks_persona_store_get_display_name(ps);
        QString storeId = QString::fromUtf8(folks_persona_store_get_id(ps));
        QString accountId("0");
        if (EDSF_IS_PERSONA_STORE(ps)) {
            ESource *source = edsf_persona_store_get_source(EDSF_PERSONA_STORE(ps));
            if (e_source_has_extension(source, E_SOURCE_EXTENSION_UBUNTU)) {
                ESourceUbuntu *ubuntu_ex = E_SOURCE_UBUNTU(e_source_get_extension(source, E_SOURCE_EXTENSION_UBUNTU));
                if (ubuntu_ex) {
                    accountId = QString::number(e_source_ubuntu_get_account_id(ubuntu_ex));
                }
            }
        }

        target.setDetailUri(QString(id).replace(":","."));
        target.setSyncTarget(displayName);
        target.setValue(QContactSyncTarget::FieldSyncTarget + 1, storeId);
        target.setValue(QContactSyncTarget::FieldSyncTarget + 2, accountId);
        details << target;
    }
    return details;
}

void QIndividual::appendDetailsForPersona(QtContacts::QContact *contact,
                                          const QtContacts::QContactDetail &detail,
                                          bool readOnly) const
{
    if (!detail.isEmpty()) {
        QtContacts::QContactDetail cpy(detail);
        QtContacts::QContactDetail::AccessConstraints access;
        if (readOnly ||
            detail.accessConstraints().testFlag(QContactDetail::ReadOnly)) {
            access |= QContactDetail::ReadOnly;
        }

        if (detail.accessConstraints().testFlag(QContactDetail::Irremovable)) {
            access |= QContactDetail::Irremovable;
        }

        QContactManagerEngine::setDetailAccessConstraints(&cpy, access);
        contact->appendDetail(cpy);
    }
}

void QIndividual::appendDetailsForPersona(QtContacts::QContact *contact,
                                          QList<QtContacts::QContactDetail> details,
                                          const QString &preferredAction,
                                          const QtContacts::QContactDetail &preferred,
                                          bool readOnly) const
{
    Q_FOREACH(const QContactDetail &detail, details) {
        appendDetailsForPersona(contact, detail, readOnly);
        if (!preferred.isEmpty()) {
            contact->setPreferredDetail(preferredAction, preferred);
        }
    }
}

QContactDetail QIndividual::getTimeStamp(FolksPersona *persona, int index) const
{
    if (!EDSF_IS_PERSONA(persona)) {
        return QContactDetail();
    }

    QContactTimestamp timestamp;
    EContact *c = edsf_persona_get_contact(EDSF_PERSONA(persona));
    const gchar *rev = static_cast<const gchar*>(e_contact_get_const(c, E_CONTACT_REV));
    if (rev) {
        QString time = QString::fromUtf8(rev);
        QDateTime rev = QDateTime::fromString(time, Qt::ISODate);
        // time is saved on UTC FORMAT
        rev.setTimeSpec(Qt::UTC);
        timestamp.setLastModified(rev);
    }

    EVCardAttribute *attr = e_vcard_get_attribute(E_VCARD(c), X_CREATED_AT);
    QDateTime createdAtDate;
    if (attr) {
        GString *createdAt = e_vcard_attribute_get_value_decoded(attr);
        createdAtDate = QDateTime::fromString(createdAt->str, Qt::ISODate);
        createdAtDate.setTimeSpec(Qt::UTC);
        g_string_free(createdAt, TRUE);
    } else {
        // use last modified data as created date if it does not exists on contact
        createdAtDate = timestamp.lastModified();
    }
    timestamp.setCreated(createdAtDate);

    return timestamp;
}


QContactDetail QIndividual::getPersonaName(FolksPersona *persona, int index) const
{
    if (!FOLKS_IS_NAME_DETAILS(persona)) {
        return QContactDetail();
    }

    QContactName detail;
    FolksStructuredName *sn = folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(persona));
    if (sn) {
        const char *name = folks_structured_name_get_given_name(sn);
        if (name && strlen(name)) {
            detail.setFirstName(qStringFromGChar(name));
        }
        name = folks_structured_name_get_additional_names(sn);
        if (name && strlen(name)) {
            detail.setMiddleName(qStringFromGChar(name));
        }
        name = folks_structured_name_get_family_name(sn);
        if (name && strlen(name)) {
            detail.setLastName(qStringFromGChar(name));
        }
        name = folks_structured_name_get_prefixes(sn);
        if (name && strlen(name)) {
            detail.setPrefix(qStringFromGChar(name));
        }
        name = folks_structured_name_get_suffixes(sn);
        if (name && strlen(name)) {
            detail.setSuffix(qStringFromGChar(name));
        }
        detail.setDetailUri(QString("%1.1").arg(index));
    }
    return detail;
}

QtContacts::QContactDetail QIndividual::getPersonaFullName(FolksPersona *persona, int index) const
{
    if (!FOLKS_IS_NAME_DETAILS(persona)) {
        return QContactDetail();
    }

    QContactDisplayLabel detail;
    const gchar *fullName = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(persona));
    if (fullName) {
        detail.setLabel(qStringFromGChar(fullName));
        detail.setDetailUri(QString("%1.1").arg(index));
    }

    return detail;
}

QtContacts::QContactDetail QIndividual::getPersonaNickName(FolksPersona *persona, int index) const
{
    if (!FOLKS_IS_NAME_DETAILS(persona)) {
        return QContactDetail();
    }

    QContactNickname detail;
    const gchar* nickname = folks_name_details_get_nickname(FOLKS_NAME_DETAILS(persona));
    if (nickname && strlen(nickname)) {
        detail.setNickname(qStringFromGChar(nickname));
        detail.setDetailUri(QString("%1.1").arg(index));
    }
    return detail;
}

QtContacts::QContactDetail QIndividual::getPersonaBirthday(FolksPersona *persona, int index) const
{
    if (!FOLKS_IS_BIRTHDAY_DETAILS(persona)) {
        return QContactDetail();
    }

    QContactBirthday detail;
    GDateTime* datetime = folks_birthday_details_get_birthday(FOLKS_BIRTHDAY_DETAILS(persona));
    if (datetime) {
        QDate date(g_date_time_get_year(datetime), g_date_time_get_month(datetime), g_date_time_get_day_of_month(datetime));
        QTime time(g_date_time_get_hour(datetime), g_date_time_get_minute(datetime), g_date_time_get_second(datetime));
        detail.setDateTime(QDateTime(date, time));
        detail.setDetailUri(QString("%1.1").arg(index));
    }
    return detail;
}

void QIndividual::folksIndividualChanged(FolksIndividual *individual,
                                         GParamSpec *pspec,
                                         QIndividual *self)
{
    Q_UNUSED(individual);
    Q_UNUSED(pspec);

    // skip update contact during a contact update, the update will be done after
    if (self->m_contactLock.tryLock()) {
        // invalidate contact
        self->markAsDirty();
        self->notifyUpdate();
        self->m_contactLock.unlock();
    }
}

QString QIndividual::qStringFromGChar(const gchar *str)
{
    return QString::fromUtf8(str).remove(QRegExp("[\r\n]"));
}

QtContacts::QContactDetail QIndividual::getPersonaPhoto(FolksPersona *persona, int index) const
{
    QContactAvatar avatar;
    if (!FOLKS_IS_AVATAR_DETAILS(persona)) {
        return avatar;
    }

    GLoadableIcon *avatarIcon = folks_avatar_details_get_avatar(FOLKS_AVATAR_DETAILS(persona));
    if (avatarIcon) {
        QString url;
        if (G_IS_FILE_ICON(avatarIcon)) {
            GFile *avatarFile = g_file_icon_get_file(G_FILE_ICON(avatarIcon));
            gchar *uri = g_file_get_uri(avatarFile);
            if (uri) {
                url = QString::fromUtf8(uri);
                g_free(uri);
            }
        } else {
            FolksAvatarCache *cache = folks_avatar_cache_dup();
            const char *contactId = folks_individual_get_id(m_individual);
            gchar *uri = folks_avatar_cache_build_uri_for_avatar(cache, contactId);
            url = QString::fromUtf8(uri);
            if (!QFile::exists(url)) {
                folks_avatar_cache_store_avatar(cache,
                                                contactId,
                                                avatarIcon,
                                                QIndividual::avatarCacheStoreDone,
                                                strdup(uri));
            }
            g_free(uri);
            g_object_unref(cache);
        }
        // Avoid to set a empty url
        if (url.isEmpty()) {
            return avatar;
        }
        avatar.setImageUrl(QUrl(url));
        avatar.setDetailUri(QString("%1.1").arg(index));
    }
    return avatar;
}

void QIndividual::avatarCacheStoreDone(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = 0;
    gchar *uri = folks_avatar_cache_store_avatar_finish(FOLKS_AVATAR_CACHE(source),
                                                        result,
                                                        &error);

    if (error) {
        qWarning() << "Fail to store avatar" << error->message;
        g_error_free(error);
    }

    if (!g_str_equal(data, uri)) {
        qWarning() << "Avatar name changed from" << (gchar*)data << "to" << uri;
    }
    g_free(data);
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaRoles(FolksPersona *persona,
                                                               QtContacts::QContactDetail *preferredRole,
                                                               int index) const
{
    if (!FOLKS_IS_ROLE_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeSet *roles = folks_role_details_get_roles(FOLKS_ROLE_DETAILS(persona));
    if (!roles) {
        return details;
    }
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(roles));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iter));
        FolksRole *role = FOLKS_ROLE(folks_abstract_field_details_get_value(fd));
        QContactOrganization org;
        QString field;

        field = qStringFromGChar(folks_role_get_organisation_name(role));
        if (!field.isEmpty()) {
            org.setName(field);
        }
        field = qStringFromGChar(folks_role_get_title(role));
        if (!field.isEmpty()) {
            org.setTitle(field);
        }
        field = qStringFromGChar(folks_role_get_role(role));
        if (!field.isEmpty()) {
            org.setRole(field);
        }
        bool isPref = false;
        DetailContextParser::parseParameters(org, fd, &isPref);
        org.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
        if (isPref) {
            *preferredRole = org;
        }

        g_object_unref(fd);
        details << org;
    }
    g_object_unref(iter);
    return details;
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaEmails(FolksPersona *persona,
                                                                QtContacts::QContactDetail *preferredEmail,
                                                                int index) const
{
    if (!FOLKS_IS_EMAIL_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeSet *emails = folks_email_details_get_email_addresses(FOLKS_EMAIL_DETAILS(persona));
    if (!emails) {
        return details;
    }
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(emails));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iter));

        const gchar *email = (const gchar*) folks_abstract_field_details_get_value(fd);

        QContactEmailAddress addr;
        addr.setEmailAddress(qStringFromGChar(email));
        bool isPref = false;
        DetailContextParser::parseParameters(addr, fd, &isPref);
        addr.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
        if (isPref) {
            *preferredEmail = addr;
        }

        g_object_unref(fd);
        details << addr;
    }
    g_object_unref(iter);
    return details;
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaPhones(FolksPersona *persona,
                                                                QtContacts::QContactDetail *preferredPhone,
                                                                int index) const
{
    if (!FOLKS_IS_PHONE_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeSet *phones = folks_phone_details_get_phone_numbers(FOLKS_PHONE_DETAILS(persona));
    if (!phones) {
        return details;
    }
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(phones));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iter));
        const gchar *phone = (const char*) folks_abstract_field_details_get_value(fd);

        QContactPhoneNumber number;
        number.setNumber(qStringFromGChar(phone));
        bool isPref = false;
        DetailContextParser::parseParameters(number, fd, &isPref);
        number.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
        if (isPref) {
            *preferredPhone = number;
        }

        g_object_unref(fd);
        details << number;
    }
    g_object_unref(iter);
    return details;
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaAddresses(FolksPersona *persona,
                                                                   QtContacts::QContactDetail *preferredAddress,
                                                                   int index) const
{
    if (!FOLKS_IS_POSTAL_ADDRESS_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeSet *addresses = folks_postal_address_details_get_postal_addresses(FOLKS_POSTAL_ADDRESS_DETAILS(persona));
    if (!addresses) {
        return details;
    }
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(addresses));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iter));
        FolksPostalAddress *addr = FOLKS_POSTAL_ADDRESS(folks_abstract_field_details_get_value(fd));

        QContactAddress address;
        const char *field = folks_postal_address_get_country(addr);
        if (field && strlen(field)) {
            address.setCountry(qStringFromGChar(field));
        }

        field = folks_postal_address_get_locality(addr);
        if (field && strlen(field)) {
            address.setLocality(qStringFromGChar(field));
        }

        field = folks_postal_address_get_po_box(addr);
        if (field && strlen(field)) {
            address.setPostOfficeBox(qStringFromGChar(field));
        }

        field = folks_postal_address_get_postal_code(addr);
        if (field && strlen(field)) {
            address.setPostcode(qStringFromGChar(field));
        }

        field = folks_postal_address_get_region(addr);
        if (field && strlen(field)) {
            address.setRegion(qStringFromGChar(field));
        }

        field = folks_postal_address_get_street(addr);
        if (field && strlen(field)) {
            address.setStreet(qStringFromGChar(field));
        }

        bool isPref = false;
        DetailContextParser::parseParameters(address, fd, &isPref);
        address.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
        if (isPref) {
            *preferredAddress = address;
        }

        g_object_unref(fd);
        details << address;
    }
    g_object_unref(iter);
    return details;
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaIms(FolksPersona *persona,
                                                             QtContacts::QContactDetail *preferredIm,
                                                             int index) const
{
    if (!FOLKS_IS_IM_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeMultiMap *ims = folks_im_details_get_im_addresses(FOLKS_IM_DETAILS(persona));
    if (!ims) {
        return details;
    }
    GeeSet *keys = gee_multi_map_get_keys(ims);
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(keys));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        const gchar *key = (const gchar*) gee_iterator_get(iter);
        GeeCollection *values = gee_multi_map_get(ims, key);

        GeeIterator *iterValues = gee_iterable_iterator(GEE_ITERABLE(values));
        while(gee_iterator_next(iterValues)) {
            FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iterValues));
            const char *uri = (const char*) folks_abstract_field_details_get_value(fd);
            GeeCollection *parameters = folks_abstract_field_details_get_parameter_values(fd, "X-FOLKS-FIELD");
            if (parameters) {
                continue;
            }

            QContactOnlineAccount account;
            account.setAccountUri(qStringFromGChar(uri));
            int protocolId = DetailContextParser::accountProtocolFromString(qStringFromGChar(key));
            account.setProtocol(static_cast<QContactOnlineAccount::Protocol>(protocolId));

            bool isPref = false;
            DetailContextParser::parseParameters(account, fd, &isPref);
            account.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
            if (isPref) {
                *preferredIm = account;
            }
            g_object_unref(fd);
            details << account;
        }
        g_object_unref(iterValues);
    }

    g_object_unref(iter);

    return details;
}

QList<QtContacts::QContactDetail> QIndividual::getPersonaUrls(FolksPersona *persona,
                                                              QtContacts::QContactDetail *preferredUrl,
                                                              int index) const
{
    if (!FOLKS_IS_URL_DETAILS(persona)) {
        return QList<QtContacts::QContactDetail>();
    }

    QList<QtContacts::QContactDetail> details;
    GeeSet *urls = folks_url_details_get_urls(FOLKS_URL_DETAILS(persona));
    if (!urls) {
        return details;
    }
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(urls));
    int fieldIndex = 1;

    while(gee_iterator_next(iter)) {
        FolksAbstractFieldDetails *fd = FOLKS_ABSTRACT_FIELD_DETAILS(gee_iterator_get(iter));
        const char *url = (const char*) folks_abstract_field_details_get_value(fd);

        QContactUrl detail;
        detail.setUrl(qStringFromGChar(url));
        bool isPref = false;
        DetailContextParser::parseParameters(detail, fd, &isPref);
        detail.setDetailUri(QString("%1.%2").arg(index).arg(fieldIndex++));
        if (isPref) {
            *preferredUrl = detail;
        }
        g_object_unref(fd);
        details << detail;
    }
    g_object_unref(iter);
    return details;
}

QtContacts::QContactDetail QIndividual::getPersonaFavorite(FolksPersona *persona, int index) const
{
    if (!FOLKS_IS_FAVOURITE_DETAILS(persona)) {
        return QtContacts::QContactDetail();
    }

    QContactFavorite detail;
    detail.setFavorite(folks_favourite_details_get_is_favourite(FOLKS_FAVOURITE_DETAILS(persona)));
    detail.setDetailUri(QString("%1.%2").arg(index).arg(1));
    return detail;
}

QList<QContactDetail> QIndividual::getPersonaExtendedDetails(FolksPersona *persona, int index) const
{
    QList<QContactDetail> result;
    if (!EDSF_IS_PERSONA(persona)) {
        return result;
    }

    EContact *c = edsf_persona_get_contact(EDSF_PERSONA(persona));
    Q_FOREACH(const QString &xDetName, m_supportedExtendedDetails) {
        EVCardAttribute *attr = e_vcard_get_attribute(E_VCARD(c), xDetName.toUtf8().constData());
        if (attr) {
            GString *attrValue = e_vcard_attribute_get_value_decoded(attr);
            QContactExtendedDetail xDet;
            xDet.setName(xDetName);
            xDet.setData(QString::fromUtf8(attrValue->str));
            xDet.setDetailUri(QString("%1.1").arg(index));
            g_string_free(attrValue, true);
            result << xDet;
        }
    }

    return result;
}



QtContacts::QContact QIndividual::copy(QList<QContactDetail::DetailType> fields)
{
    return copy(contact(), fields);
}

QtContacts::QContact QIndividual::copy(const QContact &c, QList<QContactDetail::DetailType> fields)
{
    QList<QContactDetail> details;
    QContact result;

    result = c;
    if (!fields.isEmpty()) {
        // this will remove the contact details but will keep the other metadata like preferred fields
        result = c;
        result.clearDetails();

        // mandatory
        details << c.detail<QContactGuid>();
        Q_FOREACH(QContactDetail det, c.details<QContactExtendedDetail>()) {
            details << det;
        }

        // sync targets
        Q_FOREACH(QContactDetail det, c.details<QContactSyncTarget>()) {
            details << det;
        }

        if (fields.contains(QContactDetail::TypeName)) {
            details << c.detail<QContactName>();
        }


        if (fields.contains(QContactDetail::TypeDisplayLabel)) {
            details << c.detail<QContactDisplayLabel>();
        }

        if (fields.contains(QContactDetail::TypeNickname)) {
            details << c.detail<QContactNickname>();
        }

        if (fields.contains(QContactDetail::TypeBirthday)) {
            details << c.detail<QContactBirthday>();
        }

        if (fields.contains(QContactDetail::TypeAvatar)) {
            details << c.detail<QContactAvatar>();
        }

        if (fields.contains(QContactDetail::TypeOrganization)) {
            Q_FOREACH(QContactDetail det, c.details<QContactOrganization>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypeEmailAddress)) {
            Q_FOREACH(QContactDetail det, c.details<QContactEmailAddress>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypePhoneNumber)) {
            Q_FOREACH(QContactDetail det, c.details<QContactPhoneNumber>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypeAddress)) {
            Q_FOREACH(QContactDetail det, c.details<QContactAddress>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypeUrl)) {
            Q_FOREACH(QContactDetail det, c.details<QContactUrl>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypeTag)) {
            Q_FOREACH(QContactDetail det, c.details<QContactTag>()) {
                details << det;
            }
        }

        if (fields.contains(QContactDetail::TypeFavorite)) {
            Q_FOREACH(QContactDetail det, c.details<QContactFavorite>()) {
                details << det;
            }
        }

        Q_FOREACH(QContactDetail det, details) {
            result.appendDetail(det);
        }
    }

    return result;
}

QtContacts::QContact &QIndividual::contact()
{
    if (!m_contact && m_individual) {
        QMutexLocker locker(&m_contactLock);
        updatePersonas();
        // avoid change on m_contact pointer until the contact is fully loaded
        QContact contact;
        contact.setId(QContactId("qtcontacts:galera:", m_id.toUtf8()));
        updateContact(&contact);
        m_contact = new QContact(contact);
    }
    return *m_contact;
}

void QIndividual::updatePersonas()
{
    Q_FOREACH(FolksPersona *p, m_personas.values()) {
        g_object_unref(p);
    }

    GeeSet *personas = folks_individual_get_personas(m_individual);
    if (!personas) {
        Q_ASSERT(false);
        return;
    }

    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(personas));
    while(gee_iterator_next(iter)) {
        FolksPersona *persona = FOLKS_PERSONA(gee_iterator_get(iter));
        m_personas.insert(qStringFromGChar(folks_persona_get_iid(persona)), persona);
    }

    g_object_unref(iter);
}

void QIndividual::updateContact(QContact *contact) const
{
    if (!m_individual) {
        return;
    }

    contact->appendDetail(getUid());
    Q_FOREACH(QContactDetail detail, getSyncTargets()) {
        contact->appendDetail(detail);
    }

    int personaIndex = 1;
    Q_FOREACH(FolksPersona *persona, m_personas.values()) {
         Q_ASSERT(FOLKS_IS_PERSONA(persona));

        int wsize = 0;
        gchar **wproperties = folks_persona_get_writeable_properties(persona, &wsize);
        //"gender", "local-ids", "avatar", "postal-addresses", "urls", "phone-numbers", "structured-name",
        //"anti-links", "im-addresses", "is-favourite", "birthday", "notes", "roles", "email-addresses",
        //"web-service-addresses", "groups", "full-name"
        QStringList wPropList;
        for(int i=0; i < wsize; i++) {
            wPropList << wproperties[i];
        }

        // vcard only support one of these details by contact
        if (personaIndex == 1) {
            appendDetailsForPersona(contact,
                                    getTimeStamp(persona, personaIndex),
                                    true);
            appendDetailsForPersona(contact,
                                    getPersonaName(persona, personaIndex),
                                    !wPropList.contains("structured-name"));
            appendDetailsForPersona(contact,
                                    getPersonaFullName(persona, personaIndex),
                                    !wPropList.contains("full-name"));
            appendDetailsForPersona(contact,
                                    getPersonaNickName(persona, personaIndex),
                                    !wPropList.contains("structured-name"));
            appendDetailsForPersona(contact,
                                    getPersonaBirthday(persona, personaIndex),
                                    !wPropList.contains("birthday"));
            appendDetailsForPersona(contact,
                                    getPersonaPhoto(persona, personaIndex),
                                    !wPropList.contains("avatar"));
            appendDetailsForPersona(contact,
                                    getPersonaFavorite(persona, personaIndex),
                                    !wPropList.contains("is-favourite"));
        }

        QList<QContactDetail> details;
        QContactDetail prefDetail;
        details = getPersonaRoles(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactOrganization::Type],
                                prefDetail,
                                !wPropList.contains("roles"));

        details = getPersonaEmails(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactEmailAddress::Type],
                                prefDetail,
                                !wPropList.contains("email-addresses"));

        details = getPersonaPhones(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactPhoneNumber::Type],
                                prefDetail,
                                !wPropList.contains("phone-numbers"));

        details = getPersonaAddresses(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactAddress::Type],
                                prefDetail,
                                !wPropList.contains("postal-addresses"));

        details = getPersonaIms(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactOnlineAccount::Type],
                                prefDetail,
                                !wPropList.contains("im-addresses"));

        details = getPersonaUrls(persona, &prefDetail, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                VCardParser::PreferredActionNames[QContactUrl::Type],
                                prefDetail,
                                !wPropList.contains("urls"));

        details = getPersonaExtendedDetails (persona, personaIndex);
        appendDetailsForPersona(contact,
                                details,
                                QString(),
                                QContactDetail(),
                                false);

        personaIndex++;
    }

    // Display label is mandatory
    QContactDisplayLabel dLabel = contact->detail<QContactDisplayLabel>();
    if (dLabel.label().isEmpty()) {
        dLabel.setLabel(displayName(*contact));
        contact->saveDetail(&dLabel);
    }
    QString label = dLabel.label();
    // WORKAROUND: add a extra tag to help on alphabetic list
    // On the Ubuntu Address Book, contacts which the name starts with
    // number or symbol should be moved to bottom of the list. Since the standard
    // string sort put symbols and numbers on the top, we use the tag to sort,
    // and keep empty tags for the especial case.
    QContactTag tag = contact->detail<QContactTag>();
    label = label.toUpper();
    if (label.isEmpty() ||
        !label.at(0).isLetter()) {
        tag.setTag("");
    } else {
        tag.setTag(label);
    }
    contact->saveDetail(&tag);

    QContactExtendedDetail normalizedLabel;
    normalizedLabel.setName("X-NORMALIZED_FN");
    normalizedLabel.setData(unaccent(dLabel.label()));
    contact->saveDetail(&normalizedLabel);
}

bool QIndividual::update(const QtContacts::QContact &newContact, QObject *object, const char *slot)
{
    QContact &originalContact = contact();
    if (newContact != originalContact) {
        m_currentUpdate = new UpdateContactRequest(newContact, this, object, slot);
        if (!m_contactLock.tryLock(5000)) {
            qWarning() << "Fail to lock contact to update";
            m_currentUpdate->notifyError("Fail to update contact");
            m_currentUpdate->deleteLater();
            m_currentUpdate = 0;
            return false;
        }

        m_updateConnection = QObject::connect(m_currentUpdate,
                                              &UpdateContactRequest::done,
                                              [this] (const QString &errorMessage) {

            if (errorMessage.isEmpty()) {
                markAsDirty();
            }
            m_currentUpdate->deleteLater();
            m_currentUpdate = 0;
            m_contactLock.unlock();
        });
        m_currentUpdate->start();
        return true;
    } else {
        qDebug() << "Contact is equal";
        return false;
    }
}

bool QIndividual::update(const QString &vcard, QObject *object, const char *slot)
{
    QContact contact = VCardParser::vcardToContact(vcard);
    return update(contact, object, slot);
}

FolksIndividual *QIndividual::individual() const
{
    return m_individual;
}

QList<FolksPersona *> QIndividual::personas() const
{
    return m_personas.values();
}

void QIndividual::clearPersonas()
{
    Q_FOREACH(FolksPersona *p, m_personas.values()) {
        g_object_unref(p);
    }
    m_personas.clear();
}

void QIndividual::clear()
{
    clearPersonas();
    if (m_individual) {
        // disconnect any previous handler
        Q_FOREACH(int handlerId, m_notifyConnections) {
            g_signal_handler_disconnect(m_individual, handlerId);
        }
        m_notifyConnections.clear();
        g_object_unref(m_individual);
        m_individual = 0;
    }

    if (m_contact) {
        delete m_contact;
        m_contact = 0;
    }
}

void QIndividual::addListener(QObject *object, const char *slot)
{
    int slotIndex = object->metaObject()->indexOfSlot(++slot);
    if (slotIndex == -1) {
        qWarning() << "Invalid slot:" << slot << "for object" << object;
    } else {
        m_listeners << qMakePair(object, object->metaObject()->method(slotIndex));
    }
}

bool QIndividual::isValid() const
{
    return (m_individual != 0);
}

void QIndividual::flush()
{
    // flush the folks persona store
    folks_persona_store_flush(folks_individual_aggregator_get_primary_store(m_aggregator), 0, 0);

    // cause the contact info to be reload
    markAsDirty();
}

bool QIndividual::markAsDeleted()
{
    QString currentDate = QDateTime::currentDateTime().toString(Qt::ISODate);
    GeeSet *personas = folks_individual_get_personas(m_individual);
    if (!personas) {
        return false;
    }

    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(personas));
    while(gee_iterator_next(iter)) {
        FolksPersona *persona = FOLKS_PERSONA(gee_iterator_get(iter));
        if (EDSF_IS_PERSONA(persona)) {
            FolksPersonaStore *store = folks_persona_get_store(persona);
            if (!EDSF_IS_PERSONA_STORE(store)) {
                continue;
            }

            GError *error = NULL;
            ESource *source = edsf_persona_store_get_source(EDSF_PERSONA_STORE(store));
            EClient *client = E_BOOK_CLIENT_CONNECT_SYNC(source, NULL, &error);
            if (error) {
                qWarning() << "Fail to connect with EDS" << error->message;
                g_error_free(error);
                continue;
            }

            EContact *c = edsf_persona_get_contact(EDSF_PERSONA(persona));
            EVCardAttribute *attr = e_vcard_get_attribute(E_VCARD(c), X_DELETED_AT);
            if (!attr) {
                attr = e_vcard_attribute_new("", X_DELETED_AT);
                e_vcard_add_attribute_with_value(E_VCARD(c), attr,
                                                 currentDate.toUtf8().constData());
            } else {
                e_vcard_attribute_add_value(attr, currentDate.toUtf8().constData());
            }

            e_book_client_modify_contact_sync(E_BOOK_CLIENT(client), c, NULL, &error);
            if (error) {
                qWarning() << "Fail to update EDS contact:" << error->message;
                g_error_free(error);
            } else {
                m_deletedAt = QDateTime::currentDateTime();
                notifyUpdate();
            }

            g_object_unref(client);
        }
        m_personas.insert(qStringFromGChar(folks_persona_get_iid(persona)), persona);
    }
    g_object_unref(iter);

    return m_deletedAt.isValid();
}

QDateTime QIndividual::deletedAt()
{
    if (!m_deletedAt.isNull()) {
        return m_deletedAt;
    }

    // make the date invalid to avoid re-check
    // it will be null again if the QIndividual is marked as dirty
    m_deletedAt = QDateTime(QDate(), QTime(0, 0, 0));

    GeeSet *personas = folks_individual_get_personas(m_individual);
    if (!personas) {
        return m_deletedAt;
    }

    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(personas));
    while(gee_iterator_next(iter)) {
        FolksPersona *persona = FOLKS_PERSONA(gee_iterator_get(iter));
        if (EDSF_IS_PERSONA(persona)) {
            EContact *c = edsf_persona_get_contact(EDSF_PERSONA(persona));
            EVCardAttribute *attr = e_vcard_get_attribute(E_VCARD(c), X_DELETED_AT);
            if (attr) {
                GString *value = e_vcard_attribute_get_value_decoded(attr);
                if (value) {
                    m_deletedAt = QDateTime::fromString(value->str, Qt::ISODate);
                    g_string_free(value, true);
                    // Addressbook server does not support aggregation we can return
                    // the first person value
                    break;
                }
            }
        }
    }

    return m_deletedAt;
}

bool QIndividual::setVisible(bool visible)
{
    m_visible = visible;
}

bool QIndividual::isVisible() const
{
    return m_visible;
}

void QIndividual::setIndividual(FolksIndividual *individual)
{
    static QList<QByteArray> individualProperties;

    if (m_individual != individual) {
        clear();

        if (individual) {
            QString newId = QString::fromUtf8(folks_individual_get_id(individual));
            if (!m_id.isEmpty()) {
                // we can only update to individual with the same id
                Q_ASSERT(newId == m_id);
            } else {
                m_id = newId;
            }
        }

        m_individual = individual;
        if (m_individual) {
            g_object_ref(m_individual);

            if (individualProperties.isEmpty()) {
                individualProperties << "alias"
                                     << "avatar"
                                     << "birthday"
                                     << "calendar-event-id"
                                     << "call-interaction-count"
                                     << "client-types"
                                     << "email-addresses"
                                     << "full-name"
                                     << "gender"
                                     << "groups"
                                     << "id"
                                     << "im-addresses"
                                     << "im-interaction-count"
                                     << "is-favourite"
                                     << "is-user"
                                     << "last-call-interaction-datetime"
                                     << "last-im-interaction-datetime"
                                     << "local-ids"
                                     << "location"
                                     << "nickname"
                                     << "notes"
                                     << "personas"
                                     << "phone-numbers"
                                     << "postal-addresses"
                                     << "presence-message"
                                     << "presence-status"
                                     << "presence-type"
                                     << "roles"
                                     << "structured-name"
                                     << "trust-level"
                                     << "urls"
                                     << "web-service-addresses";
            }

            Q_FOREACH(const QByteArray &property, individualProperties) {
                uint signalHandler = g_signal_connect(G_OBJECT(m_individual), QByteArray("notify::") + property,
                                                      (GCallback) QIndividual::folksIndividualChanged,
                                                      const_cast<QIndividual*>(this));
                m_notifyConnections << signalHandler;
            }
        }
    }
}

GHashTable *QIndividual::parseAddressDetails(GHashTable *details,
                                             const QList<QtContacts::QContactDetail> &cDetails,
                                             const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value = GeeUtils::gValueSliceNew(G_TYPE_OBJECT);
    GeeCollection *collection = GEE_COLLECTION(SET_AFD_NEW());
    g_value_take_object(value, collection);

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        if(!detail.isEmpty()) {
            QContactAddress address = static_cast<QContactAddress>(detail);
            FolksPostalAddress *postalAddress = folks_postal_address_new(address.postOfficeBox().toUtf8().data(),
                                                                         NULL, // extension
                                                                         address.street().toUtf8().data(),
                                                                         address.locality().toUtf8().data(),
                                                                         address.region().toUtf8().data(),
                                                                         address.postcode().toUtf8().data(),
                                                                         address.country().toUtf8().data(),
                                                                         NULL,  // address format
                                                                         NULL); //UID


            if (!folks_postal_address_is_empty(postalAddress)) {
                FolksPostalAddressFieldDetails *pafd = folks_postal_address_field_details_new(postalAddress, NULL);
                DetailContextParser::parseContext(FOLKS_ABSTRACT_FIELD_DETAILS(pafd),
                                                  address,
                                                  detail == prefDetail);
                gee_collection_add(collection, pafd);
                g_object_unref(pafd);
            }

            g_object_unref(postalAddress);
        }
    }

    GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_POSTAL_ADDRESSES, value);

    return details;
}

GHashTable *QIndividual::parsePhotoDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactAvatar avatar = static_cast<QContactAvatar>(detail);
        if(!avatar.isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(G_TYPE_FILE_ICON);
            QUrl avatarUri = avatar.imageUrl();
            if(!avatarUri.isEmpty()) {
                QString formattedUri = avatarUri.toString(QUrl::RemoveUserInfo);
                if(!formattedUri.isEmpty()) {
                    GFile *avatarFile = g_file_new_for_uri(formattedUri.toUtf8().data());
                    GFileIcon *avatarFileIcon = G_FILE_ICON(g_file_icon_new(avatarFile));
                    g_value_take_object(value, avatarFileIcon);

                    GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_AVATAR, value);
                    g_clear_object((GObject**) &avatarFile);
                }
            } else {
                g_object_unref(value);
            }
        }
    }

    return details;
}

GHashTable *QIndividual::parseBirthdayDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactBirthday birthday = static_cast<QContactBirthday>(detail);
        if(!birthday.isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(G_TYPE_DATE_TIME);
            GDateTime *dateTime = g_date_time_new_from_unix_utc(birthday.dateTime().toMSecsSinceEpoch() / 1000);
            g_value_set_boxed(value, dateTime);

            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_BIRTHDAY, value);
            g_date_time_unref(dateTime);
        }
    }

    return details;
}

GHashTable *QIndividual::parseEmailDetails(GHashTable *details,
                                           const QList<QtContacts::QContactDetail> &cDetails,
                                           const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value;
    PERSONA_DETAILS_INSERT_STRING_FIELD_DETAILS(details, cDetails,
                                                FOLKS_PERSONA_DETAIL_EMAIL_ADDRESSES, value, QContactEmailAddress,
                                                FOLKS_TYPE_EMAIL_FIELD_DETAILS, emailAddress, prefDetail);
    return details;
}

GHashTable *QIndividual::parseFavoriteDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactFavorite favorite = static_cast<QContactFavorite>(detail);
        if(!favorite.isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(G_TYPE_BOOLEAN);
            g_value_set_boolean(value, favorite.isFavorite());

            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_IS_FAVOURITE, value);
        }
    }

    return details;
}

GHashTable *QIndividual::parseGenderDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactGender gender = static_cast<QContactDetail>(detail);
        if(!gender.isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(FOLKS_TYPE_GENDER);
            FolksGender genderEnum = FOLKS_GENDER_UNSPECIFIED;
            if(gender.gender() == QContactGender::GenderMale) {
                genderEnum = FOLKS_GENDER_MALE;
            } else if(gender.gender() == QContactGender::GenderFemale) {
                genderEnum = FOLKS_GENDER_FEMALE;
            }
            g_value_set_enum(value, genderEnum);

            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_GENDER, value);
        }
    }

    return details;
}

GHashTable *QIndividual::parseNameDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactName name = static_cast<QContactName>(detail);
        if(!name.isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(FOLKS_TYPE_STRUCTURED_NAME);
            FolksStructuredName *sn = folks_structured_name_new(name.lastName().toUtf8().data(),
                                                                name.firstName().toUtf8().data(),
                                                                name.middleName().toUtf8().data(),
                                                                name.prefix().toUtf8().data(),
                                                                name.suffix().toUtf8().data());
            g_value_take_object(value, sn);
            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_STRUCTURED_NAME, value);
        }
    }

    return details;
}

GHashTable *QIndividual::parseFullNameDetails(GHashTable *details,
                                              const QList<QtContacts::QContactDetail> &cDetails,
                                              const QString &fallback)
{
    bool found = false;
    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactDisplayLabel displayLabel = static_cast<QContactDisplayLabel>(detail);
        if(!displayLabel.label().isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(G_TYPE_STRING);
            g_value_set_string(value, displayLabel.label().toUtf8().data());
            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_FULL_NAME, value);
            found = true;
        }
    }

    // Full name is mandatory
    if (!found) {
        GValue *value = GeeUtils::gValueSliceNew(G_TYPE_STRING);
        g_value_set_string(value, fallback.toUtf8().data());
        GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_FULL_NAME, value);
    }
    return details;
}

GHashTable *QIndividual::parseNicknameDetails(GHashTable *details, const QList<QtContacts::QContactDetail> &cDetails)
{
    if(cDetails.size() == 0) {
        return details;
    }

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactNickname nickname = static_cast<QContactNickname>(detail);
        if(!nickname.nickname().isEmpty()) {
            GValue *value = GeeUtils::gValueSliceNew(G_TYPE_STRING);
            g_value_set_string(value, nickname.nickname().toUtf8().data());
            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_NICKNAME, value);

            // FIXME: check if those values should all be set to the same thing
            value = GeeUtils::gValueSliceNew(G_TYPE_STRING);
            g_value_set_string(value, nickname.nickname().toUtf8().data());
            GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_ALIAS, value);
        }
    }

    return details;
}

GHashTable *QIndividual::parseNoteDetails(GHashTable *details,
                                          const QList<QtContacts::QContactDetail> &cDetails,
                                          const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value;
    PERSONA_DETAILS_INSERT_STRING_FIELD_DETAILS(details, cDetails,
                                                FOLKS_PERSONA_DETAIL_NOTES, value, QContactNote,
                                                FOLKS_TYPE_NOTE_FIELD_DETAILS, note, prefDetail);

    return details;
}

GHashTable *QIndividual::parseImDetails(GHashTable *details,
                                        const QList<QtContacts::QContactDetail> &cDetails,
                                        const QtContacts::QContactDetail &prefDetail)
{
    Q_UNUSED(prefDetail);

    if(cDetails.size() == 0) {
        return details;
    }

    QMultiMap<QString, QString> providerUidMap;
    Q_FOREACH(const QContactDetail &detail, cDetails) {
        QContactOnlineAccount account = static_cast<QContactOnlineAccount>(detail);
        if (!account.isEmpty()) {
            providerUidMap.insert(DetailContextParser::accountProtocolName(account.protocol()), account.accountUri());
        }
    }

    if(!providerUidMap.isEmpty()) {
        //TODO: add account type and subtype
        GValue *value = GeeUtils::asvSetStrNew(providerUidMap);
        GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_IM_ADDRESSES, value);
    }

    return details;
}

GHashTable *QIndividual::parseOrganizationDetails(GHashTable *details,
                                                  const QList<QtContacts::QContactDetail> &cDetails,
                                                  const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value = GeeUtils::gValueSliceNew(G_TYPE_OBJECT);
    GeeCollection *collection = GEE_COLLECTION(SET_AFD_NEW());
    g_value_take_object(value, collection);

    Q_FOREACH(const QContactDetail& detail, cDetails) {
        QContactOrganization org = static_cast<QContactOrganization>(detail);
        if(!org.isEmpty()) {
            FolksRole *role = folks_role_new(org.title().toUtf8().data(),
                                             org.name().toUtf8().data(),
                                             NULL);
            folks_role_set_role(role, org.role().toUtf8().data());
            FolksRoleFieldDetails *rfd = folks_role_field_details_new(role, NULL);
            DetailContextParser::parseContext(FOLKS_ABSTRACT_FIELD_DETAILS(rfd), org, detail == prefDetail);
            gee_collection_add(collection, rfd);

            g_object_unref(rfd);
            g_object_unref(role);
        }
    }
    GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_ROLES, value);

    return details;
}

GHashTable *QIndividual::parsePhoneNumbersDetails(GHashTable *details,
                                                  const QList<QtContacts::QContactDetail> &cDetails,
                                                  const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value = GeeUtils::gValueSliceNew(G_TYPE_OBJECT);
    Q_FOREACH(const QContactDetail &detail, cDetails) {
        QContactPhoneNumber phone = static_cast<QContactPhoneNumber>(detail);
        if(!phone.isEmpty()) {
            gValueGeeSetAddStringFieldDetails(value,
                                              FOLKS_TYPE_PHONE_FIELD_DETAILS,
                                              phone.number().toUtf8().data(),
                                              phone, detail == prefDetail);
        }
    }
    GeeUtils::personaDetailsInsert(details, FOLKS_PERSONA_DETAIL_PHONE_NUMBERS, value);

    return details;
}

GHashTable *QIndividual::parseUrlDetails(GHashTable *details,
                                         const QList<QtContacts::QContactDetail> &cDetails,
                                         const QtContacts::QContactDetail &prefDetail)
{
    if(cDetails.size() == 0) {
        return details;
    }

    GValue *value;
    PERSONA_DETAILS_INSERT_STRING_FIELD_DETAILS(details, cDetails,
                                                FOLKS_PERSONA_DETAIL_URLS, value, QContactUrl,
                                                FOLKS_TYPE_URL_FIELD_DETAILS, url, prefDetail);

    return details;
}

GHashTable *QIndividual::parseDetails(const QtContacts::QContact &contact)
{
    GHashTable *details = g_hash_table_new_full(g_str_hash,
                                                g_str_equal,
                                                NULL,
                                                (GDestroyNotify) GeeUtils::gValueSliceFree);

    parsePhotoDetails(details, contact.details(QContactAvatar::Type));
    parseBirthdayDetails(details, contact.details(QContactBirthday::Type));
    parseFavoriteDetails(details, contact.details(QContactFavorite::Type));
    parseGenderDetails(details, contact.details(QContactGender::Type));
    parseNameDetails(details, contact.details(QContactName::Type));
    parseFullNameDetails(details, contact.details(QContactDisplayLabel::Type), displayName(contact));
    parseNicknameDetails(details, contact.details(QContactNickname::Type));

    parseAddressDetails(details,
                        contact.details(QContactAddress::Type),
                        contact.preferredDetail(VCardParser::PreferredActionNames[QContactAddress::Type]));
    parseEmailDetails(details,
                      contact.details(QContactEmailAddress::Type),
                      contact.preferredDetail(VCardParser::PreferredActionNames[QContactEmailAddress::Type]));
    parseNoteDetails(details,
                     contact.details(QContactNote::Type),
                     contact.preferredDetail(VCardParser::PreferredActionNames[QContactNote::Type]));
    parseImDetails(details,
                   contact.details(QContactOnlineAccount::Type),
                   contact.preferredDetail(VCardParser::PreferredActionNames[QContactOnlineAccount::Type]));
    parseOrganizationDetails(details,
                             contact.details(QContactOrganization::Type),
                             contact.preferredDetail(VCardParser::PreferredActionNames[QContactOrganization::Type]));
    parsePhoneNumbersDetails(details,
                             contact.details(QContactPhoneNumber::Type),
                             contact.preferredDetail(VCardParser::PreferredActionNames[QContactPhoneNumber::Type]));
    parseUrlDetails(details,
                    contact.details(QContactUrl::Type),
                    contact.preferredDetail(VCardParser::PreferredActionNames[QContactUrl::Type]));

    QContactDisplayLabel label = contact.detail<QContactDisplayLabel>();
    if (label.label().isEmpty()) {
        // display label is mandatory, usese the fallback in case of empty values
        label.setLabel(displayName(contact));

    }
    return details;
}

QString QIndividual::displayName(const QContact &contact)
{
    if (contact.isEmpty()) {
        return "";
    }

    QString fallbackLabel;
    // format display name based on designer request
    // fallback priority list [Name, Company, PhoneNumber, Email, Alias]
    if (fallbackLabel.isEmpty()) {
        QContactName name = contact.detail<QContactName>();
        if (!name.isEmpty()) {
            QStringList names;
            if (!name.prefix().isEmpty()) {
                names << name.prefix();
            }
            if (!name.firstName().isEmpty()) {
                names << name.firstName();
            }
            if (!name.middleName().isEmpty()) {
                names << name.middleName();
            }
            if (!name.lastName().isEmpty()) {
                names << name.lastName();
            }
            if (!name.suffix().isEmpty()) {
                names << name.suffix();
            }

            fallbackLabel = names.join(" ").trimmed();
        }
    }

    if (fallbackLabel.isEmpty()) {
        QContactOrganization org = contact.detail<QContactOrganization>();
        fallbackLabel = org.name().trimmed();
    }

    if (fallbackLabel.isEmpty()) {
        QContactPhoneNumber number = contact.detail<QContactPhoneNumber>();
        fallbackLabel = number.number().trimmed();
    }

    if (fallbackLabel.isEmpty()) {
        QContactEmailAddress email = contact.detail<QContactEmailAddress>();
        fallbackLabel = email.emailAddress().trimmed();
    }

    if (fallbackLabel.isEmpty()) {
        QContactOnlineAccount account = contact.detail<QContactOnlineAccount>();
        fallbackLabel = account.accountUri().trimmed();
    }

    return fallbackLabel;
}

void QIndividual::setExtendedDetails(FolksPersona *persona,
                                     const QList<QContactDetail> &xDetails,
                                     const QDateTime &createdAtDate)
{
    FolksPersonaStore *store = folks_persona_get_store(persona);
    if (EDSF_IS_PERSONA_STORE(store)) {
        GError *error = NULL;
        ESource *source = edsf_persona_store_get_source(EDSF_PERSONA_STORE(store));
        EClient *client = E_BOOK_CLIENT_CONNECT_SYNC(source, NULL, &error);
        if (error) {
            qWarning() << "Fail to connect with EDS" << error->message;
            g_error_free(error);
        } else {
            EContact *c = edsf_persona_get_contact(EDSF_PERSONA(persona));

            // create X-CREATED-AT if it does not exists
            EVCardAttribute *attr = e_vcard_get_attribute(E_VCARD(c), X_CREATED_AT);
            if (!attr) {
                QDateTime createdAt = createdAtDate.isValid() ? createdAtDate : QDateTime::currentDateTime();
                attr = e_vcard_attribute_new("", X_CREATED_AT);
                e_vcard_add_attribute_with_value(E_VCARD(c),
                                                 attr,
                                                 createdAt.toUTC().toString(Qt::ISODate).toUtf8().constData());
            }

            Q_FOREACH(const QContactDetail &d, xDetails) {
                QContactExtendedDetail xd = static_cast<QContactExtendedDetail>(d);
                // X_CREATED_AT should not be updated
                if (xd.name() == X_CREATED_AT) {
                    continue;
                }

                if (m_supportedExtendedDetails.contains(xd.name())) {
                    // Remove old attribute
                    attr = e_vcard_get_attribute(E_VCARD(c), xd.name().toUtf8().constData());
                    if (attr) {
                        e_vcard_remove_attribute(E_VCARD(c), attr);
                    }

                    attr = e_vcard_attribute_new("", xd.name().toUtf8().constData());
                    e_vcard_add_attribute_with_value(E_VCARD(c),
                                                     attr,
                                                     xd.data().toString().toUtf8().constData());
                } else {
                    qWarning() << "Extended detail not supported" << xd.name();
                }
            }

            e_book_client_modify_contact_sync(E_BOOK_CLIENT(client), c, NULL, &error);
            if (error) {
                qWarning() << "Fail to update EDS contact:" << error->message;
                g_error_free(error);
            }
        }
        g_object_unref(client);
    }
}

void QIndividual::markAsDirty()
{
    delete m_contact;
    m_contact = 0;
    m_deletedAt = QDateTime();
}

void QIndividual::enableAutoLink(bool flag)
{
    m_autoLink = flag;
}

bool QIndividual::autoLinkEnabled()
{
    return m_autoLink;
}

FolksPersona* QIndividual::primaryPersona()
{
    if (m_personas.size() > 0) {
        return m_personas.begin().value();
    } else {
        return 0;
    }
}

QtContacts::QContactDetail QIndividual::detailFromUri(QtContacts::QContactDetail::DetailType type, const QString &uri) const
{
    Q_FOREACH(QContactDetail detail, m_contact->details(type)) {
        if (detail.detailUri() == uri) {
            return detail;
        }
    }
    return QContactDetail();
}

} //namespace

