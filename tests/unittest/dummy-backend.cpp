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

#include "config.h"
#include "dummy-backend.h"
#include "scoped-loop.h"

#include "lib/qindividual.h"
#include "common/vcard-parser.h"

#include <QtCore/QDir>
#include <QtCore/QDebug>


DummyBackendProxy::DummyBackendProxy()
    : m_adaptor(0),
      m_backend(0),
      m_primaryPersonaStore(0),
      m_backendStore(0),
      m_aggregator(0),
      m_isReady(false),
      m_individualsChangedDetailedId(0)
{
}

DummyBackendProxy::~DummyBackendProxy()
{
    shutdown();
}

void DummyBackendProxy::start(bool useDBus)
{
    m_useDBus = useDBus;
    initEnviroment();
    initFolks();
}

void DummyBackendProxy::shutdown()
{
    m_isReady = false;
    if (m_adaptor) {
        QDBusConnection connection = QDBusConnection::sessionBus();
        connection.unregisterObject(DUMMY_OBJECT_PATH);
        connection.unregisterService(DUMMY_SERVICE_NAME);

        delete m_adaptor;
        m_adaptor = 0;
        Q_EMIT stopped();
    }

    Q_FOREACH(galera::QIndividual *i, m_contacts.values()) {
        delete i;
    }
    m_contacts.clear();

    if (m_aggregator) {
        g_signal_handler_disconnect(m_aggregator,
                                    m_individualsChangedDetailedId);

        g_object_unref(m_aggregator);
        m_aggregator = 0;
    }

    if (m_primaryPersonaStore) {
        g_object_unref(m_primaryPersonaStore);
        m_primaryPersonaStore = 0;
    }

    if (m_backend) {
        g_object_unref(m_backend);
        m_backend = 0;
    }

    if (m_backendStore) {
        g_object_unref(m_backendStore);
        m_backendStore = 0;
    }
}
QList<QtContacts::QContact> DummyBackendProxy::contacts() const
{
    QList<QtContacts::QContact> contacts;
    Q_FOREACH(galera::QIndividual *i, m_contacts.values()) {
        contacts << i->contact();
    }
    return contacts;
}

QList<galera::QIndividual *> DummyBackendProxy::individuals() const
{
    return m_contacts.values();
}

FolksIndividualAggregator *DummyBackendProxy::aggregator() const
{
    return m_aggregator;
}

QStringList DummyBackendProxy::listContacts() const
{
    return galera::VCardParser::contactToVcardSync(contacts());
}

void DummyBackendProxy::reset()
{
    if (m_contacts.count()) {
        GeeMap *map = folks_persona_store_get_personas(m_primaryPersonaStore);
        GeeCollection *personas = gee_map_get_values(map);
        folks_dummy_persona_store_unregister_personas(FOLKS_DUMMY_PERSONA_STORE(m_primaryPersonaStore), (GeeSet*)personas);
        g_object_unref(personas);
        m_contacts.clear();
    }

    // remove any extra collection/persona store
    GeeHashSet *extraStores = gee_hash_set_new(FOLKS_TYPE_PERSONA_STORE,
                                                 (GBoxedCopyFunc) g_object_ref, g_object_unref,
                                                 NULL, NULL, NULL, NULL, NULL, NULL);

    GeeMap *currentStores = folks_backend_get_persona_stores(FOLKS_BACKEND(m_backend));
    GeeSet *keys = gee_map_get_keys(currentStores);
    GeeIterator *iter = gee_iterable_iterator(GEE_ITERABLE(keys));

    while(gee_iterator_next(iter)) {
        const gchar *key = (const gchar*) gee_iterator_get(iter);
        if (strcmp(key, "dummy-store") != 0) {
            FolksPersonaStore *store = FOLKS_PERSONA_STORE(gee_map_get(currentStores, key));
            gee_abstract_collection_add(GEE_ABSTRACT_COLLECTION(extraStores), store);
            g_object_unref(store);
        }
    }

    if (gee_collection_get_size(GEE_COLLECTION(extraStores)) > 0) {
        folks_dummy_backend_unregister_persona_stores(m_backend, GEE_SET(extraStores));
    }

    g_object_unref(extraStores);
    g_object_unref(keys);
    g_object_unref(iter);
}

void DummyBackendProxy::initFolks()
{
    m_aggregator = folks_individual_aggregator_dup();
    m_individualsChangedDetailedId = g_signal_connect(m_aggregator,
                                          "individuals-changed-detailed",
                                          (GCallback) DummyBackendProxy::individualsChangedCb,
                                          this);
    folks_individual_aggregator_prepare(m_aggregator,
                                        (GAsyncReadyCallback) DummyBackendProxy::individualAggregatorPrepared,
                                        this);
}

bool DummyBackendProxy::isReady() const
{
    return m_isReady;
}

QString DummyBackendProxy::createContact(const QtContacts::QContact &qcontact)
{
    ScopedEventLoop loop(&m_eventLoop);

    GHashTable *details = galera::QIndividual::parseDetails(qcontact);
    Q_ASSERT(details);
    Q_ASSERT(m_aggregator);
    folks_individual_aggregator_add_persona_from_details(m_aggregator,
                                                         NULL, //parent
                                                         m_primaryPersonaStore,
                                                         details,
                                                         (GAsyncReadyCallback) DummyBackendProxy::individualAggregatorAddedPersona,
                                                         this);

    loop.exec();
    g_hash_table_destroy(details);
    return QString();
}

void DummyBackendProxy::contactUpdated(const QString &contactId,
                                       const QString &errorMsg)
{
    m_contactUpdated = true;
}

QString DummyBackendProxy::updateContact(const QString &contactId,
                                         const QtContacts::QContact &qcontact)
{
    galera::QIndividual *i = m_contacts.value(contactId);
    Q_ASSERT(i);
    ScopedEventLoop loop(&m_eventLoop);
    m_contactUpdated = false;
    i->update(qcontact, this, SLOT(contactUpdated(QString,QString)));
    loop.exec();
    return i->id();
}

void DummyBackendProxy::checkError(GError *error)
{
    if (error) {
        qWarning() << error->message;
        g_error_free(error);
    }
    Q_ASSERT(error == 0);
}

void DummyBackendProxy::mkpath(const QString &path) const
{
    QDir dir;
    if (!dir.mkpath(path)) {
        qWarning() << "Fail to create path" << path;
    }
}

void DummyBackendProxy::initEnviroment()
{
    Q_ASSERT(m_tmpDir.isValid());
    QString tmpFullPath = QString("%1").arg(m_tmpDir.path());

    qputenv("FOLKS_BACKENDS_ALLOWED", "dummy");
    qputenv("FOLKS_PRIMARY_STORE", "dummy");

    mkpath(tmpFullPath);
    qDebug() << "setting up in transient directory:" << tmpFullPath;

    // home
    qputenv("HOME", tmpFullPath.toUtf8().data());

    // cache
    QString cacheDir = QString("%1/.cache/").arg(tmpFullPath);
    mkpath(cacheDir);
    qputenv("XDG_CACHE_HOME", cacheDir.toUtf8().data());

    // config
    QString configDir = QString("%1/.config").arg(tmpFullPath);
    mkpath(configDir);
    qputenv("XDG_CONFIG_HOME", configDir.toUtf8().data());

    // data
    QString dataDir = QString("%1/.local/share").arg(tmpFullPath);
    mkpath(dataDir);
    qputenv("XDG_DATA_HOME", dataDir.toUtf8().data());
    mkpath(QString("%1/folks").arg(dataDir));

    // runtime
    QString runtimeDir = QString("%1/run").arg(tmpFullPath);
    mkpath(runtimeDir);
    qputenv("XDG_RUNTIME_DIR", runtimeDir.toUtf8().data());

    qputenv("XDG_DESKTOP_DIR", "");
    qputenv("XDG_DOCUMENTS_DIR", "");
    qputenv("XDG_DOWNLOAD_DIR", "");
    qputenv("XDG_MUSIC_DIR", "");
    qputenv("XDG_PICTURES_DIR", "");
    qputenv("XDG_PUBLICSHARE_DIR", "");
    qputenv("XDG_TEMPLATES_DIR", "");
    qputenv("XDG_VIDEOS_DIR", "");
}

bool DummyBackendProxy::registerObject()
{
    QDBusConnection connection = QDBusConnection::sessionBus();

    if (!connection.registerService(DUMMY_SERVICE_NAME)) {
        qWarning() << "Could not register service!" << DUMMY_SERVICE_NAME;
        return false;
    }

    m_adaptor = new DummyBackendAdaptor(connection, this);
    if (!connection.registerObject(DUMMY_OBJECT_PATH, this))
    {
        qWarning() << "Could not register object!" << DUMMY_OBJECT_PATH;
        delete m_adaptor;
        m_adaptor = 0;
    } else {
        qDebug() << "Object registered:" << QString(DUMMY_OBJECT_PATH);
    }

    return (m_adaptor != 0);
}

void DummyBackendProxy::individualAggregatorPrepared(FolksIndividualAggregator *fia,
                                                     GAsyncResult *res,
                                                     DummyBackendProxy *self)
{
    GError *error = 0;

    folks_individual_aggregator_prepare_finish(fia, res, &error);
    checkError(error);

    self->m_backendStore = folks_backend_store_dup();
    self->m_backend = FOLKS_DUMMY_BACKEND(folks_backend_store_dup_backend_by_name(self->m_backendStore, "dummy"));
    if (!self->m_backend) {
        qWarning() << "fail to load dummy backend";
    }

    self->m_primaryPersonaStore = folks_individual_aggregator_get_primary_store(fia);
    g_object_ref(self->m_primaryPersonaStore);

    if (self->m_useDBus) {
        self->registerObject();
    }

    self->m_isReady = true;
    Q_EMIT self->ready();
}

void DummyBackendProxy::individualAggregatorAddedPersona(FolksIndividualAggregator *fia,
                                                         GAsyncResult *res,
                                                         DummyBackendProxy *self)
{
    GError *error = 0;
    folks_individual_aggregator_add_persona_from_details_finish(fia, res, &error);
    checkError(error);

    self->m_eventLoop->quit();
    self->m_eventLoop = 0;
}

void DummyBackendProxy::individualsChangedCb(FolksIndividualAggregator *individualAggregator,
                                             GeeMultiMap *changes,
                                             DummyBackendProxy *self)
{
    Q_UNUSED(individualAggregator);

    GeeIterator *iter;
    GeeSet *removed = gee_multi_map_get_keys(changes);
    GeeCollection *added = gee_multi_map_get_values(changes);
    QStringList addedIds;

    iter = gee_iterable_iterator(GEE_ITERABLE(added));
    while(gee_iterator_next(iter)) {
        FolksIndividual *individual = FOLKS_INDIVIDUAL(gee_iterator_get(iter));
        if (individual) {
            galera::QIndividual *idv = new galera::QIndividual(individual, self->m_aggregator);
            self->m_contacts.insert(idv->id(), idv);
            addedIds << idv->id();
            g_object_unref(individual);
        }
    }
    g_object_unref (iter);

    iter = gee_iterable_iterator(GEE_ITERABLE(removed));
    while(gee_iterator_next(iter)) {
        FolksIndividual *individual = FOLKS_INDIVIDUAL(gee_iterator_get(iter));
        if (individual) {
            QString id = QString::fromUtf8(folks_individual_get_id(individual));
            if (!addedIds.contains(id) && self->m_contacts.contains(id)) {
                delete self->m_contacts.take(id);
            }
            g_object_unref(individual);
        }
    }
    g_object_unref (iter);

    g_object_unref(added);
    g_object_unref(removed);
}

DummyBackendAdaptor::DummyBackendAdaptor(const QDBusConnection &connection, DummyBackendProxy *parent)
    : QDBusAbstractAdaptor(parent),
      m_connection(connection),
      m_proxy(parent)
{
    if (m_proxy->isReady()) {
        Q_EMIT ready();
    }
    connect(m_proxy, SIGNAL(ready()), this, SIGNAL(ready()));
}

DummyBackendAdaptor::~DummyBackendAdaptor()
{
}

bool DummyBackendAdaptor::isReady()
{
    return m_proxy->isReady();
}

bool DummyBackendAdaptor::ping()
{
    return true;
}

void DummyBackendAdaptor::quit()
{
    QMetaObject::invokeMethod(m_proxy, "shutdown", Qt::QueuedConnection);
}

void DummyBackendAdaptor::reset()
{
    m_proxy->reset();
}

QStringList DummyBackendAdaptor::listContacts()
{
    return m_proxy->listContacts();
}

QString DummyBackendAdaptor::createContact(const QString &vcard)
{
    QtContacts::QContact contact = galera::VCardParser::vcardToContact(vcard);
    return m_proxy->createContact(contact);
}

QString DummyBackendAdaptor::updateContact(const QString &contactId, const QString &vcard)
{
    QtContacts::QContact contact = galera::VCardParser::vcardToContact(vcard);
    return m_proxy->updateContact(contactId, contact);
}

void DummyBackendAdaptor::enableAutoLink(bool flag)
{
    galera::QIndividual::enableAutoLink(flag);
}

