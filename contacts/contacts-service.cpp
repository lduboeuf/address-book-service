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

#include "contacts-service.h"
#include "qcontactcollectionfetchrequest-data.h"
#include "qcontactrequest-data.h"
#include "qcontactfetchrequest-data.h"
#include "qcontactfetchbyidrequest-data.h"
#include "qcontactremoverequest-data.h"
#include "qcontactsaverequest-data.h"

#include "common/vcard-parser.h"
#include "common/filter.h"
#include "common/fetch-hint.h"
#include "common/sort-clause.h"
#include "common/dbus-service-defs.h"
#include "common/source.h"

#include <QtCore/QSharedPointer>

#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusConnectionInterface>

#include <QtContacts/QContact>
#include <QtContacts/QContactChangeSet>
#include <QtContacts/QContactCollectionFetchRequest>
#include <QtContacts/QContactName>
#include <QtContacts/QContactPhoneNumber>
#include <QtContacts/QContactFilter>

#include <QtVersit/QVersitReader>
#include <QtVersit/QVersitContactImporter>
#include <QtVersit/QVersitContactExporter>
#include <QtVersit/QVersitWriter>

#define ALTERNATIVE_CPIM_SERVICE_PAGE_SIZE  "CANONICAL_PIM_SERVICE_PAGE_SIZE"
#define FETCH_PAGE_SIZE                     25

using namespace QtVersit;
using namespace QtContacts;

namespace galera
{
GaleraContactsService::GaleraContactsService(const QString &managerUri)
    : m_managerUri(managerUri),
      m_serviceIsReady(false),
      m_iface(0)
{
    Source::registerMetaType();

    if (qEnvironmentVariableIsSet(ALTERNATIVE_CPIM_SERVICE_NAME)) {
        m_serviceName = qgetenv(ALTERNATIVE_CPIM_SERVICE_NAME);
    } else {
        m_serviceName = CPIM_SERVICE_NAME;
    }

    if (qEnvironmentVariableIsSet(ALTERNATIVE_CPIM_SERVICE_PAGE_SIZE)) {
        m_pageSize = qgetenv(ALTERNATIVE_CPIM_SERVICE_PAGE_SIZE).toInt();
    } else {
        m_pageSize = FETCH_PAGE_SIZE;
    }

    m_serviceWatcher = new QDBusServiceWatcher(m_serviceName,
                                               QDBusConnection::sessionBus(),
                                               QDBusServiceWatcher::WatchForOwnerChange,
                                               this);
    connect(m_serviceWatcher, SIGNAL(serviceOwnerChanged(QString,QString,QString)),
            this, SLOT(serviceOwnerChanged(QString,QString,QString)));

    initialize();
}

GaleraContactsService::GaleraContactsService(const GaleraContactsService &other)
    : m_managerUri(other.m_managerUri),
      m_iface(other.m_iface)
{
}

GaleraContactsService::~GaleraContactsService()
{
    delete m_serviceWatcher;
    Q_FOREACH(QContactRequestData *r, m_runningRequests) {
        r->cancel();
        r->wait();
    }

    m_runningRequests.clear();
}

void GaleraContactsService::serviceOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    Q_UNUSED(oldOwner);
    if (name == m_serviceName) {
        if (!newOwner.isEmpty()) {
            // service appear
            qDebug() << "Service appeared";
            initialize();
        } else if (!m_iface.isNull()) {
            // lost service
            qDebug() << "Service disappeared";
            deinitialize();
        }
    }
}

void GaleraContactsService::onServiceReady()
{
    bool isReady = m_iface.data()->property("isReady").toBool();
    if (isReady != m_serviceIsReady) {
        m_serviceIsReady = isReady;
        Q_EMIT serviceChanged();
    }
}

void GaleraContactsService::initialize()
{
    if (m_iface.isNull()) {
        m_iface = QSharedPointer<QDBusInterface>(new QDBusInterface(m_serviceName,
                                                                    CPIM_ADDRESSBOOK_OBJECT_PATH,
                                                                    CPIM_ADDRESSBOOK_IFACE_NAME));
        if (!m_iface->lastError().isValid()) {
            m_serviceIsReady = m_iface.data()->property("isReady").toBool();
            connect(m_iface.data(), SIGNAL(readyChanged()), this, SLOT(onServiceReady()), Qt::UniqueConnection);
            connect(m_iface.data(), SIGNAL(safeModeChanged()), this, SIGNAL(serviceChanged()));
            connect(m_iface.data(), SIGNAL(contactsAdded(QStringList)), this, SLOT(onContactsAdded(QStringList)));
            connect(m_iface.data(), SIGNAL(contactsRemoved(QStringList)), this, SLOT(onContactsRemoved(QStringList)));
            connect(m_iface.data(), SIGNAL(contactsUpdated(QStringList)), this, SLOT(onContactsUpdated(QStringList)));
            if (m_serviceIsReady) {
                Q_EMIT serviceChanged();
            }
        } else {
            qWarning() << "Fail to connect with service:"  << m_iface->lastError();
            m_iface.clear();
        }
    }
}

void GaleraContactsService::deinitialize()
{
    // wait until all request finish
    while (m_runningRequests.size()) {
        QCoreApplication::processEvents();
    }

    // this will make the service re-initialize
    m_iface->call("ping");

    if (m_iface->lastError().isValid()) {
        qWarning() << m_iface->lastError();
        m_iface.clear();
        m_serviceIsReady = false;
    } else {
        m_serviceIsReady = m_iface.data()->property("isReady").toBool();
    }

    Q_EMIT serviceChanged();
}

bool GaleraContactsService::isOnline() const
{
    return !m_iface.isNull() && m_serviceIsReady;
}

void GaleraContactsService::fetchCollections(QContactCollectionFetchRequest *request)
{
    if (!isOnline()) {
        qWarning() << "Server is not online";
        QContactCollectionFetchRequestData::notifyError(request);
        return;
    }

    QDBusPendingCall pcall = m_iface->asyncCall("availableSources");
    if (pcall.isError()) {
        qWarning() << pcall.error().name() << pcall.error().message();
        QContactCollectionFetchRequestData::notifyError(request);
        return;
    }

    QContactCollectionFetchRequestData *data =
        new QContactCollectionFetchRequestData(request);
    m_runningRequests << data;

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
    data->updateWatcher(watcher);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
        this->fetchCollectionsContinue(data, call);
    });
}

void GaleraContactsService::fetchCollectionsContinue(QContactCollectionFetchRequestData *data,
                                                     QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<SourceList> reply = *call;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        destroyRequest(data);
    } else {
        QList<QContactCollection> collections;
        Q_FOREACH(const Source &source, reply.value()) {
            QContactCollection collection = source.toCollection();
            collection.setId(QContactCollectionId(m_managerUri,
                                                  source.id().toUtf8()));
            collections.append(collection);
        }
        data->update(collections,
                     QContactAbstractRequest::FinishedState,
                     QContactManager::NoError);
    }
}

void GaleraContactsService::fetchContactsById(QtContacts::QContactFetchByIdRequest *request)
{
    if (!isOnline()) {
        qWarning() << "Server is not online";
        QContactFetchByIdRequestData::notifyError(request);
        return;
    }

    QContactIdFilter filter;
    filter.setIds(request->contactIds());
    QString filterStr = Filter(filter).toString();
    QDBusMessage result = m_iface->call("query",
                                        filterStr, "",
                                        request->fetchHint().maxCountHint(),
                                        m_showInvisibleContacts,
                                        QStringList());
    if (result.type() == QDBusMessage::ErrorMessage) {
        qWarning() << result.errorName() << result.errorMessage();
        QContactFetchByIdRequestData::notifyError(request);
        return;
    }

    QDBusObjectPath viewObjectPath = result.arguments()[0].value<QDBusObjectPath>();
    QDBusInterface *view = new QDBusInterface(m_serviceName,
                                             viewObjectPath.path(),
                                             CPIM_ADDRESSBOOK_VIEW_IFACE_NAME);

    QContactFetchByIdRequestData *data = new QContactFetchByIdRequestData(request, view);
    m_runningRequests << data;
    fetchContactsPage(data);
}

void GaleraContactsService::fetchContacts(QtContacts::QContactFetchRequest *request)
{
    if (!isOnline()) {
        qWarning() << "Server is not online";
        QContactFetchRequestData::notifyError(request);
        return;
    }

    // Only return the sources names if the filter is set as contact group type
    if (request->filter().type() == QContactFilter::ContactDetailFilter) {
        QContactDetailFilter dFilter = static_cast<QContactDetailFilter>(request->filter());

        if ((dFilter.detailType() == QContactDetail::TypeType) &&
            (dFilter.detailField() == QContactType::FieldType) &&
            (dFilter.value() == QContactType::TypeGroup)) {

            QDBusPendingCall pcall = m_iface->asyncCall("availableSources");
            if (pcall.isError()) {
                qWarning() << pcall.error().name() << pcall.error().message();
                QContactFetchRequestData::notifyError(request);
                return;
            }

            QContactFetchRequestData *data = new QContactFetchRequestData(request, 0);
            m_runningRequests << data;

            QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
            data->updateWatcher(watcher);
            QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                             [=](QDBusPendingCallWatcher *call) {
                                this->fetchContactsGroupsContinue(data, call);
                             });
            return;
        }
    }

    QString sortStr = SortClause(request->sorting()).toString();
    QString filterStr = Filter(request->filter()).toString();
    FetchHint fetchHint = FetchHint(request->fetchHint()).toString();
    QDBusPendingCall pcall = m_iface->asyncCall("query",
                                                filterStr,
                                                sortStr,
                                                request->fetchHint().maxCountHint(),
                                                m_showInvisibleContacts,
                                                QStringList());
    if (pcall.isError()) {
        qWarning() << pcall.error().name() << pcall.error().message();
        QContactFetchRequestData::notifyError(request);
        return;
    }

    QContactFetchRequestData *data = new QContactFetchRequestData(request, 0, fetchHint);
    m_runningRequests << data;

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
    data->updateWatcher(watcher);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
                        this->fetchContactsContinue(data, call);
                     });
}


void GaleraContactsService::fetchContactsContinue(QContactFetchRequestData *data,
                                                  QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<QDBusObjectPath> reply = *call;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        destroyRequest(data);
    } else {
        QDBusObjectPath viewObjectPath = reply.value();
        QDBusInterface *view = new QDBusInterface(m_serviceName,
                                                  viewObjectPath.path(),
                                                  CPIM_ADDRESSBOOK_VIEW_IFACE_NAME);
        data->updateView(view);
        fetchContactsPage(data);
    }
}

void GaleraContactsService::fetchContactsPage(QContactFetchRequestData *data)
{
    if (!isOnline() || !data->isLive()) {
        destroyRequest(data);
        return;
    }

    // Load contacs async
    QDBusPendingCall pcall = data->view()->asyncCall("contactsDetails",
                                                     data->fields(),
                                                     data->offset(),
                                                     m_pageSize);
    if (pcall.isError()) {
        qWarning() << pcall.error().name() << pcall.error().message();
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
    data->updateWatcher(watcher);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
                        this->fetchContactsDone(data, call);
                     });
}

void GaleraContactsService::fetchContactsDone(QContactFetchRequestData *data,
                                              QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<QStringList> reply = *call;
    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        data->update(QList<QContact>(),
                        QContactAbstractRequest::FinishedState,
                        QContactManager::UnspecifiedError);
        destroyRequest(data);
    } else {
        const QStringList vcards = reply.value();
        if (vcards.size()) {
            VCardParser *parser = new VCardParser;
            parser->setProperty("DATA", QVariant::fromValue<void*>(data));
            data->setVCardParser(parser);
            connect(parser,
                    SIGNAL(contactsParsed(QList<QtContacts::QContact>)),
                    SLOT(onVCardsParsed(QList<QtContacts::QContact>)));
            connect(parser,
                    SIGNAL(canceled()),
                    SLOT(onVCardParseCanceled()));
            parser->vcardToContact(vcards);
        } else {
            data->update(QList<QContact>(), QContactAbstractRequest::FinishedState);
            destroyRequest(data);
        }
    }
}

void GaleraContactsService::onVCardParseCanceled()
{
    QObject *sender = QObject::sender();
    disconnect(sender);

    QContactFetchRequestData *data = static_cast<QContactFetchRequestData*>(sender->property("DATA").value<void*>());
    data->clearVCardParser();

    if (!data->isLive()) {
        sender->deleteLater();
        destroyRequest(data);
        return;
    }

    sender->deleteLater();
}

void GaleraContactsService::onVCardsParsed(QList<QContact> contacts)
{
    QObject *sender = QObject::sender();
    disconnect(sender);

    QContactFetchRequestData *data = static_cast<QContactFetchRequestData*>(sender->property("DATA").value<void*>());
    data->clearVCardParser();

    if (!data->isLive()) {
        sender->deleteLater();
        destroyRequest(data);
        return;
    }

    QList<QContact>::iterator contact;
    for (contact = contacts.begin(); contact != contacts.end(); ++contact) {
        if (!contact->isEmpty()) {
            QContactGuid detailId = contact->detail<QContactGuid>();
            QContactId newId(m_managerUri, detailId.guid().toUtf8());
            contact->setId(newId);
        }
    }

    if (contacts.size() == m_pageSize) {
        data->update(contacts, QContactAbstractRequest::ActiveState);
        data->updateOffset(m_pageSize);
        data->updateWatcher(0);
        fetchContactsPage(data);
    } else {
        data->update(contacts, QContactAbstractRequest::FinishedState);
        destroyRequest(data);
    }

    sender->deleteLater();
}

void GaleraContactsService::fetchContactsGroupsContinue(QContactFetchRequestData *data,
                                                        QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QList<QContact> contacts;
    QContactManager::Error opError = QContactManager::NoError;

    QDBusPendingReply<SourceList> reply = *call;
    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        opError = QContactManager::UnspecifiedError;
    } else {
        Q_FOREACH(const Source &source, reply.value()) {
            QContactId id(m_managerUri, QByteArray("source@") + source.id().toUtf8());
            QContact c = source.toContact(id);
            if (source.isPrimary()) {
                contacts.prepend(c);
            } else {
                contacts << c;
            }
        }
    }

    data->update(contacts, QContactAbstractRequest::FinishedState, opError);
    destroyRequest(data);
}

/* Saving contacts
 *
 * Due the limitation on QtPim API we do not have a native way to create
 * 'address-books'/'sources'/'collections', to WORKAROUND it we use contacts
 * with type == 'QContactType::TypeGroup' as 'collections'
 *
 * FIXME: the new QtPim API already support collections for contacts. We should
 * rewrite this before update to the new QtPim.
 *
 * The steps are:
 *  - Create each group individually due the limitation of the server API
 *  - Create each contact individually due the limitation of the sever API
 *  - Update all groups that already have IDs
 *  - Update all contacts that already have IDs
 *
 * If the request was canceled between any of these steps, the data object is destroyed and a finish signal is fired.
 */

/* This function can receive a mix of contacts and groups, the contacts without id will be created
 * on server and contacts that already have id will be updated:
 */
void GaleraContactsService::saveContact(QtContacts::QContactSaveRequest *request)
{
    QContactSaveRequestData *data = new QContactSaveRequestData(request);
    m_runningRequests << data;

    // first create the new groups
    data->prepareToCreate();
    createGroupsStart(data);
}

/* Will create async the first group on the list, if the list is not empty,
 * otherwise it will call 'createContactsStart' to create the contacts in the list
 *
 * Due the Server limitation we will call this function for each contact
 * sequentially.
 */
void GaleraContactsService::createGroupsStart(QContactSaveRequestData *data)
{
    if (!data->isLive()) {
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    if(!data->hasNextGroup()) {
        // If there is no more groups to create go to create contacts
        createContactsStart(data);
        return;
    }

    Source sourceToCreate = data->nextGroup();
    QDBusPendingCall pcall = m_iface->asyncCall("createSourceForAccount",
                                                sourceToCreate.displayLabel(),
                                                sourceToCreate.accountId(),
                                                sourceToCreate.isPrimary());
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
    data->updateWatcher(watcher);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
                        this->createGroupDone(data, call);
                     });
}

/* 'createSourceForAccount' will call this function when done,
 * we need to check for errors and update the contact Id with the new Id, and
 * call 'createGroupsStart' to continue with the next group.
 */
void GaleraContactsService::createGroupDone(QContactSaveRequestData *data,
                                            QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<Source> reply = *call;
    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        data->notifyUpdateError(QContactManager::UnspecifiedError);
    } else {
        data->updateCurrentGroup(reply.value(), m_managerUri);
    }

    // go to next group
    createGroupsStart(data);
}

/* After handle all contacts with type = 'QContactType::TypeGroup', we need to
 * create the real contacts.
 *
 * Due the Server limitation we will call this function for each contact
 * sequentially.
 */
void GaleraContactsService::createContactsStart(QContactSaveRequestData *data)
{
    if (!data->isLive()) {
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    if(!data->hasNext()) {
        // If there is no more contacts to create go to update groups
        data->prepareToUpdate();
        updateGroups(data);
        return;
    }

    QString syncSource;
    QString contact = data->nextContact(&syncSource);

    QDBusPendingCall pcall = m_iface->asyncCall("createContact", contact, syncSource);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
    data->updateWatcher(watcher);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     [=](QDBusPendingCallWatcher *call) {
                        this->createContactsDone(data, call);
                     });
}

/* 'createContact' will call this function when done,
 * we need to check for errors and update the contact Id with the new Id, and
 * call 'createContactsStart' to continue with the next contact.
  */
void GaleraContactsService::createContactsDone(QContactSaveRequestData *data,
                                               QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<QString> reply = *call;
    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        data->notifyUpdateError(QContactManager::UnspecifiedError);
    } else {
        const QString vcard = reply.value();
        if (!vcard.isEmpty()) {
            QContact contact = VCardParser::vcardToContact(vcard);
            QContactGuid detailId = contact.detail<QContactGuid>();
            QContactId newId(m_managerUri, detailId.guid().toUtf8());
            contact.setId(newId);
            data->updateCurrentContact(contact);
        } else {
            data->notifyUpdateError(QContactManager::UnspecifiedError);
        }
    }

    // go to next contact
    createContactsStart(data);
}

/*
 * Our server support update a list of groups, because of that we can handle all
 * pending to update groups in one single call.
 */
void GaleraContactsService::updateGroups(QContactSaveRequestData *data)
{
    if (!data->isLive()) {
        data->finish(QContactManager::UnspecifiedError);
        destroyRequest(data);
        return;
    }

    SourceList pendingGroups = data->allPendingGroups();
    if (pendingGroups.isEmpty()) {
        // If there is no groups to update we can proceed to 'updateContacts'
        updateContacts(data);
        return;
    }

    QDBusPendingCall pcall = m_iface->asyncCall("updateSources", QVariant::fromValue<SourceList>(pendingGroups));
    if (pcall.isError()) {
        qWarning() <<  "Error" << pcall.error().name() << pcall.error().message();
        data->finish(QtContacts::QContactManager::UnspecifiedError);
        destroyRequest(data);
    } else {
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
        data->updateWatcher(watcher);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                         [=](QDBusPendingCallWatcher *call) {
                            this->updateGroupsDone(data, call);
                         });
    }
}

/*
 * Callback used to process server reply from 'updateSources', we need to check
 * for errors and update the result with the new groups info.
 */
void GaleraContactsService::updateGroupsDone(QContactSaveRequestData *data, QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<SourceList> reply = *call;
    QContactManager::Error opError = QContactManager::NoError;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        opError = QContactManager::UnspecifiedError;
    } else {
        const SourceList sources = reply.value();
        data->updatePendingGroups(sources, m_managerUri);
    }

    // proceed to next step 'updateContacts'
    updateContacts(data);
}

/*
 * Last step
 * We will update all pending contacts
 */
void GaleraContactsService::updateContacts(QContactSaveRequestData *data)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QStringList pendingContacts = data->allPendingContacts();
    if (pendingContacts.isEmpty()) {
        // Last step, notify query finish and destroy query data
        data->finish(QContactManager::NoError);
        destroyRequest(data);
        return;
    }

    QDBusPendingCall pcall = m_iface->asyncCall("updateContacts", pendingContacts);
    if (pcall.isError()) {
        qWarning() <<  "Error" << pcall.error().name() << pcall.error().message();
        data->finish(QtContacts::QContactManager::UnspecifiedError);
        destroyRequest(data);
    } else {
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
        data->updateWatcher(watcher);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                         [=](QDBusPendingCallWatcher *call) {
                            this->updateContactDone(data, call);
                         });
    }
}

/*
 * Callback used to process server reply from 'updateContacts', we need to check
 * for errors and update the result with the new contact info.
 */
void GaleraContactsService::updateContactDone(QContactSaveRequestData *data, QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    QDBusPendingReply<QStringList> reply = *call;
    QContactManager::Error opError = QContactManager::NoError;

    if (reply.isError()) {
        qWarning() << reply.error().name() << reply.error().message();
        opError = QContactManager::UnspecifiedError;
    } else {
        const QStringList vcards = reply.value();
        data->updatePendingContacts(vcards);
    }

    data->finish(opError);
    // Last step of 'saveContact', we can destroy the request data now.
    destroyRequest(data);
}

void GaleraContactsService::removeContact(QContactRemoveRequest *request)
{
    if (!isOnline()) {
        qWarning() << "Server is not online";
        QContactRemoveRequestData::notifyError(request);
        return;
    }

    QContactRemoveRequestData *data = new QContactRemoveRequestData(request);
    m_runningRequests << data;

    if (data->contactIds().isEmpty()) {
        removeContactContinue(data, 0);
    } else {
        QDBusPendingCall pcall = m_iface->asyncCall("removeContacts", data->contactIds());
        if (pcall.isError()) {
            qWarning() <<  "Error" << pcall.error().name() << pcall.error().message();
            data->finish(QContactManager::UnspecifiedError);
            destroyRequest(data);
        } else {
            QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
            data->updateWatcher(watcher);
            QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                             [=](QDBusPendingCallWatcher *call) {
                                this->removeContactContinue(data, call);
                             });
        }
    }
}

void GaleraContactsService::removeContactContinue(QContactRemoveRequestData *data,
                                                  QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    if (call) {
        QDBusPendingReply<int> reply = *call;
        if (reply.isError()) {
            qWarning() << reply.error().name() << reply.error().message();
            data->finish(QContactManager::UnspecifiedError);
            destroyRequest(data);
            return;
        }
    }

    if (data->sourcesIds().isEmpty()) {
        removeContactDone(data, 0);
    } else {
        if (data->sourcesIds().size() > 1) {
            qWarning() << "Remove multiple sources not supported.";
        }

        QDBusPendingCall pcall = m_iface->asyncCall("removeSource", data->sourcesIds().first());
        if (pcall.isError()) {
            qWarning() <<  "Error" << pcall.error().name() << pcall.error().message();
            data->finish(QContactManager::UnspecifiedError);
            destroyRequest(data);
        } else {
            QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pcall, 0);
            data->updateWatcher(watcher);
            QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                             [=](QDBusPendingCallWatcher *call) {
                                this->removeContactDone(data, call);
                             });
        }
    }
}

void GaleraContactsService::removeContactDone(QContactRemoveRequestData *data,
                                              QDBusPendingCallWatcher *call)
{
    if (!data->isLive()) {
        destroyRequest(data);
        return;
    }

    if (call) {
        QDBusPendingReply<bool> reply = *call;
        if (reply.isError()) {
            qWarning() << reply.error().name() << reply.error().message();
            data->finish(QContactManager::UnspecifiedError);
            destroyRequest(data);
            return;
        }
    }
    data->finish();
    destroyRequest(data);
}


void GaleraContactsService::cancelRequest(QtContacts::QContactAbstractRequest *request)
{
    Q_FOREACH(QContactRequestData *rData, m_runningRequests) {
        if (rData->request() == request) {
            rData->cancel();
            destroyRequest(rData);
            return;
        }
    }
}

void GaleraContactsService::waitRequest(QtContacts::QContactAbstractRequest *request)
{
    QContactRequestData *data = 0;
    Q_FOREACH(QContactRequestData *rData, m_runningRequests) {
        if (rData->request() == request) {
            data = rData;
            break;
        }
    }

    if (data) {
        data->wait();

        // this is the only case where we still need to delete data even if the data is not in the running list anymore,
        // because we can not delete it while waiting (the pointer still be used by wait function)
        m_runningRequests.removeOne(data);
        // the data could be removed from m_runningRequests while waiting, but we still need to destroy it
        data->deleteLater();
    }
}

void GaleraContactsService::releaseRequest(QContactAbstractRequest *request)
{
    Q_FOREACH(QContactRequestData *rData, m_runningRequests) {
        if (rData->request() == request) {
            m_runningRequests.removeOne(rData);
            rData->releaseRequest();
            rData->cancel();
            rData->deleteLater();
            return;
        }
    }
}

void GaleraContactsService::setShowInvisibleContacts(bool show)
{
    m_showInvisibleContacts = show;
}

void GaleraContactsService::addRequest(QtContacts::QContactAbstractRequest *request)
{
    if (!isOnline()) {
        qWarning() << "Server is not online";
        QContactManagerEngine::updateRequestState(request, QContactAbstractRequest::FinishedState);
        return;
    }

    Q_ASSERT(request->state() == QContactAbstractRequest::ActiveState);
    switch (request->type()) {
        case QContactAbstractRequest::ContactFetchRequest:
            fetchContacts(static_cast<QContactFetchRequest*>(request));
            break;
        case QContactAbstractRequest::CollectionFetchRequest:
            fetchCollections(static_cast<QContactCollectionFetchRequest*>(request));
            break;
        case QContactAbstractRequest::ContactFetchByIdRequest:
            fetchContactsById(static_cast<QContactFetchByIdRequest*>(request));
            break;
        case QContactAbstractRequest::ContactIdFetchRequest:
            qDebug() << "Not implemented: ContactIdFetchRequest";
            break;
        case QContactAbstractRequest::ContactSaveRequest:
            saveContact(static_cast<QContactSaveRequest*>(request));
            break;
        case QContactAbstractRequest::ContactRemoveRequest:
            removeContact(static_cast<QContactRemoveRequest*>(request));
            break;
        case QContactAbstractRequest::RelationshipFetchRequest:
            qDebug() << "Not implemented: RelationshipFetchRequest";
            break;
        case QContactAbstractRequest::RelationshipRemoveRequest:
            qDebug() << "Not implemented: RelationshipRemoveRequest";
            break;
        case QContactAbstractRequest::RelationshipSaveRequest:
            qDebug() << "Not implemented: RelationshipSaveRequest";
            break;

        default: // unknown request type.
        break;
    }
}

void GaleraContactsService::destroyRequest(QContactRequestData *request)
{
    // only destroy the resquest data if it still on the list
    // otherwise it was already destroyed
    if (m_runningRequests.removeOne(request)) {
        request->deleteLater();
    }
}

QList<QContactId> GaleraContactsService::parseIds(const QStringList &ids) const
{
    QList<QContactId> contactIds;
    Q_FOREACH(QString id, ids) {
        contactIds << QContactId(m_managerUri, id.toUtf8());
    }
    return contactIds;
}

void GaleraContactsService::onContactsAdded(const QStringList &ids)
{
    Q_EMIT contactsAdded(parseIds(ids));
}

void GaleraContactsService::onContactsRemoved(const QStringList &ids)
{
    Q_EMIT contactsRemoved(parseIds(ids));
}

void GaleraContactsService::onContactsUpdated(const QStringList &ids)
{
    Q_EMIT contactsUpdated(parseIds(ids), {});
}

} //namespace
