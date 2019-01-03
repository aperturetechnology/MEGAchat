//we need the POSIX version of strerror_r, not the GNU one
#ifdef _GNU_SOURCE
    #undef _GNU_SOURCE
    #define _POSIX_C_SOURCE 201512L
#endif
#include <string.h>

#include "chatClient.h"
#ifdef _WIN32
    #include <winsock2.h>
    #include <direct.h>
    #define mkdir(dir, mode) _mkdir(dir)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtcModule/webrtc.h"
#ifndef KARERE_DISABLE_WEBRTC
    #include "rtcCrypto.h"
    #include "dummyCrypto.h" //for makeRandomString
#endif
#include "base/services.h"
#include "sdkApi.h"
#include <serverListProvider.h>
#include <memory>
#include <chatd.h>
#include <db.h>
#include <buffer.h>
#include <chatdDb.h>
#include <megaapi_impl.h>
#include <autoHandle.h>
#include <asyncTools.h>
#include <codecvt> //for nonWhitespaceStr()
#include <locale>
#include "strongvelope/strongvelope.h"
#include "base64url.h"
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __ANDROID__
    #include <sys/system_properties.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #ifdef TARGET_OS_IPHONE
        #include <resolv.h>
    #endif
#endif

#define _QUICK_LOGIN_NO_RTC
using namespace promise;

namespace karere
{

template <class T, class F>
void callAfterInit(T* self, F&& func, void* ctx);

std::string encodeFirstName(const std::string& first);

bool Client::anonymousMode() const
{
    return (mInitState == kInitAnonymousMode);
}

/* Warning - the database is not initialzed at construction, but only after
 * init() is called. Therefore, no code in this constructor should access or
 * depend on the database
 */
Client::Client(::mega::MegaApi& sdk, WebsocketsIO *websocketsIO, IApp& aApp, const std::string& appDir, uint8_t caps, void *ctx)
    : mAppDir(appDir),
          websocketIO(websocketsIO),
          appCtx(ctx),
          api(sdk, ctx),
          app(aApp),
          contactList(new ContactList(*this)),
          chats(new ChatRoomList(*this)),
          mPresencedClient(&api, this, *this, caps)
{
}

KARERE_EXPORT const std::string& createAppDir(const char* dirname, const char *envVarName)
{
    static std::string path;
    if (!path.empty())
        return path;
    const char* dir = getenv(envVarName);
    if (dir)
    {
        path = dir;
    }
    else
    {
        const char* homedir = getenv(
            #ifndef _WIN32
                    "HOME"
            #else
                    "HOMEPATH"
            #endif
        );
        if (!homedir)
            throw std::runtime_error("Cant get HOME env variable");
        path = homedir;
        path.append("/").append(dirname);
    }
    struct stat info;
    auto ret = stat(path.c_str(), &info);
    if (ret == 0)
    {
        if ((info.st_mode & S_IFDIR) == 0)
            throw std::runtime_error("Application directory path is taken by a file");
    }
    else
    {
        ret = mkdir(path.c_str(), 0700);
        if (ret)
        {
            char buf[512];
#ifdef _WIN32
            strerror_s(buf, 511, ret);
#else
            (void)strerror_r(ret, buf, 511);
#endif
            buf[511] = 0; //just in case
            throw std::runtime_error(std::string("Error creating application directory: ")+buf);
        }
    }
    return path;
}

std::string Client::dbPath(const std::string& sid) const
{
    std::string path = mAppDir;
    if (sid.empty())    // anonoymous-mode
    {
        path.reserve(20);
        path.append("/karere-").append("anonymous.db");
    }
    else
    {
        if (sid.size() < 50)
            throw std::runtime_error("dbPath: sid is too small");

        path.reserve(56);
        path.append("/karere-").append(sid.c_str()+44).append(".db");
    }

    return path;
}

bool Client::openDb(const std::string& sid)
{
    assert(!sid.empty());
    std::string path = dbPath(sid);
    struct stat info;
    bool exists = (stat(path.c_str(), &info) == 0);
    if (!exists)
    {
        KR_LOG_WARNING("Asked to use local cache, but it does not exist");
        return false;
    }

    bool ok = db.open(path.c_str(), false);
    if (!ok)
    {
        KR_LOG_WARNING("Error opening database");
        return false;
    }
    SqliteStmt stmt(db, "select value from vars where name = 'schema_version'");
    if (!stmt.step())
    {
        db.close();
        KR_LOG_WARNING("Can't get local database version");
        return false;
    }

    std::string currentVersion(gDbSchemaHash);
    currentVersion.append("_").append(gDbSchemaVersionSuffix);    // <hash>_<suffix>

    std::string cachedVersion(stmt.stringCol(0));
    if (cachedVersion != currentVersion)
    {
        ok = false;

        // if only version suffix changed, we may be able to provide backwards compatibility without
        // forcing a full reload, but just porting/adapting data
        size_t cachedVersionSuffixPos = cachedVersion.find_last_of('_');
        if (cachedVersionSuffixPos != std::string::npos)
        {
            std::string cachedVersionSuffix = cachedVersion.substr(cachedVersionSuffixPos + 1);
            if (cachedVersionSuffix == "2" && (strcmp(gDbSchemaVersionSuffix, "3") == 0))
            {
                KR_LOG_WARNING("Clearing history from cached chats...");

                // clients with version 2 missed the call-history msgs, need to clear cached history
                // in order to fetch fresh history including the missing management messages
                db.query("delete from history");
                db.query("update chat_vars set value = 0 where name = 'have_all_history'");
                db.query("update vars set value = ? where name = 'schema_version'", currentVersion);
                db.commit();

                KR_LOG_WARNING("Successfully cleared cached history. Database version has been updated to %s", gDbSchemaVersionSuffix);

                ok = true;
            }
            else if (cachedVersionSuffix == "3" && (strcmp(gDbSchemaVersionSuffix, "4") == 0))
            {
                // clients with version 3 need to force a full-reload of SDK's cache to retrieve
                // "deleted" chats from API, since it used to not return them. It should only be
                // done in case there's at least one chat.

                SqliteStmt stmt(db, "select count(*) from chats");
                stmt.stepMustHaveData("get chats count");
                if (stmt.intCol(0) > 0)
                {
                    KR_LOG_WARNING("Forcing a reload of SDK and MEGAchat caches...");
                    api.sdk.invalidateCache();
                }
                else    // no chats --> only invalidate MEGAchat cache (the schema has changed)
                {
                    KR_LOG_WARNING("Forcing a reload of SDK and MEGAchat cache...");
                }

                KR_LOG_WARNING("Database version has been updated to %s", gDbSchemaVersionSuffix);
            }
            else if (cachedVersionSuffix == "4" && (strcmp(gDbSchemaVersionSuffix, "5") == 0))
            {
                // clients with version 4 need to create a new table `node_history` and populate it with
                // node's attachments already in cache. Futhermore, the existing types for special messages
                // (node-attachments, contact-attachments and rich-links) will be updated to a different
                // range to avoid collissions with the types of upcoming management messages.

                // Update obsolete type of special messages
                db.query("update history set type=? where type=?", chatd::Message::Type::kMsgAttachment, 0x10);
                db.query("update history set type=? where type=?", chatd::Message::Type::kMsgRevokeAttachment, 0x11);
                db.query("update history set type=? where type=?", chatd::Message::Type::kMsgContact, 0x12);
                db.query("update history set type=? where type=?", chatd::Message::Type::kMsgContainsMeta, 0x13);

                // Create new table for node history
                db.simpleQuery("CREATE TABLE node_history(idx int not null, chatid int64 not null, msgid int64 not null,"
                               "    userid int64, keyid int not null, type tinyint, updated smallint, ts int,"
                               "    is_encrypted tinyint, data blob, backrefid int64 not null, UNIQUE(chatid,msgid), UNIQUE(chatid,idx))");

                // Populate new table with existing node-attachments
                db.query("insert into node_history select * from history where type=?", std::to_string(chatd::Message::Type::kMsgAttachment));
                int count = sqlite3_changes(db);

                // Update DB version number
                db.query("update vars set value = ? where name = 'schema_version'", currentVersion);
                db.commit();

                KR_LOG_WARNING("Database version has been updated to %s", gDbSchemaVersionSuffix);
                KR_LOG_WARNING("%d messages added to node history", count);
                ok = true;
            }
            else if (cachedVersionSuffix == "5" && (strcmp(gDbSchemaVersionSuffix, "6") == 0))
            {
                // Clients with version 5 need to force a full-reload of SDK's in case there's at least one group chat.
                // Otherwise the cache schema must be updated to support public chats

                SqliteStmt stmt(db, "select count(*) from chats where peer == -1");
                stmt.stepMustHaveData("get chats count");
                if (stmt.intCol(0) > 0)
                {
                    KR_LOG_WARNING("Forcing a reload of SDK and MEGAchat caches...");
                    api.sdk.invalidateCache();
                }
                else
                {
                    // no chats --> only update cache schema
                    db.query("ALTER TABLE `chats` ADD mode tinyint");
                    db.query("ALTER TABLE `chats` ADD unified_key blob");
                    KR_LOG_WARNING("Updating cache schema ...");
                    ok = true;
                }

                KR_LOG_WARNING("Database version has been updated to %s", gDbSchemaVersionSuffix);
            }
        }
    }

    if (!ok)
    {
        db.close();
        KR_LOG_WARNING("Database schema version is not compatible with app version, will rebuild it");
        return false;
    }

    mSid = sid;
    return true;
}

void Client::createDbSchema()
{
    mMyHandle = Id::inval();
    db.simpleQuery(gDbSchema); //db.query() uses a prepared statement and will execute only the first statement up to the first semicolon
    std::string ver(gDbSchemaHash);
    ver.append("_").append(gDbSchemaVersionSuffix);
    db.query("insert into vars(name, value) values('schema_version', ?)", ver);
    db.commit();
}

void Client::heartbeat()
{
    if (db.isOpen())
    {
        db.timedCommit();
    }

    if (mConnState != kConnected)
    {
        KR_LOG_WARNING("Heartbeat timer tick without being connected");
        return;
    }

    mPresencedClient.heartbeat();
    if (mChatdClient)
    {
        mChatdClient->heartbeat();
    }
}

Client::~Client()
{
    assert(isTerminated());
}

void Client::retryPendingConnections(bool disconnect)
{
    if (mConnState == kDisconnected)  // already a connection attempt in-progress
    {
        KR_LOG_WARNING("Retry pending connections called without previous connect");
        return;
    }

    mPresencedClient.retryPendingConnection(disconnect);
    if (mChatdClient)
    {
        mChatdClient->retryPendingConnections(disconnect);
    }
}

#define TOKENPASTE2(a,b) a##b
#define TOKENPASTE(a,b) TOKENPASTE2(a,b)

#define SHARED_STATE(varname, membtype)             \
    struct TOKENPASTE(SharedState,__LINE__){membtype value;};  \
    std::shared_ptr<TOKENPASTE(SharedState, __LINE__)> varname(new TOKENPASTE(SharedState,__LINE__))

//This is a convenience method to log in the SDK in case the app does not do it.
promise::Promise<void> Client::sdkLoginNewSession()
{
    mLoginDlg.assign(app.createLoginDialog());
    async::loop((int)0, [](int) { return true; }, [](int&){},
    [this](async::Loop<int>& loop)
    {
        auto pms = mLoginDlg->requestCredentials();
        return pms
        .then([this](const std::pair<std::string, std::string>& cred)
        {
            mLoginDlg->setState(IApp::ILoginDialog::kLoggingIn);
            return api.callIgnoreResult(&mega::MegaApi::login, cred.first.c_str(), cred.second.c_str());
        })
        .then([&loop]()
        {
            loop.breakLoop();
            return 0;
        })
        .fail([this](const promise::Error& err) -> Promise<int>
        {
            if (err.code() != mega::API_ENOENT && err.code() != mega::API_EARGS)
                return err;

            mLoginDlg->setState(IApp::ILoginDialog::kBadCredentials);
            return 0;
        });
    }, this)
    .then([this](int)
    {
        mLoginDlg->setState(IApp::ILoginDialog::kFetchingNodes);
        return api.callIgnoreResult(&::mega::MegaApi::fetchNodes);
    })
    .fail([this](const promise::Error& err)
    {
        auto wptr = weakHandle();
        marshallCall([wptr, this, err]()
        {
            if (wptr.deleted())
            {
                return;
            }

            mSessionReadyPromise.reject(err);
        }, appCtx);
    })
    .then([this]()
    {
        mLoginDlg.free();
    });
    return mSessionReadyPromise;
}

promise::Promise<void> Client::sdkLoginExistingSession(const char* sid)
{
    assert(sid);
    api.callIgnoreResult(&::mega::MegaApi::fastLogin, sid)
    .then([this]()
    {
        api.callIgnoreResult(&::mega::MegaApi::fetchNodes);
    });
    return mSessionReadyPromise;
}


promise::Promise<ReqResult> Client::openChatPreview(uint64_t publicHandle)
{
    auto wptr = weakHandle();
    return api.call(&::mega::MegaApi::getChatLinkURL, publicHandle)
    .then([this, wptr](ReqResult result) -> promise::Promise<ReqResult>
    {
        wptr.throwIfDeleted();
        return result;
    });
}

void Client::createPublicChatRoom(uint64_t chatId, uint64_t ph, int shard, int numPeers, const std::string &decryptedTitle, std::shared_ptr<std::string> unifiedKey, const std::string &url, uint32_t ts)
{
    GroupChatRoom *room = new GroupChatRoom(*chats, chatId, shard, chatd::Priv::PRIV_RDONLY, ts, false, decryptedTitle, ph, unifiedKey, numPeers, url);
    chats->emplace(chatId, room);
    room->connect(url.c_str());
}

promise::Promise<std::string> Client::decryptChatTitle(uint64_t chatId, const std::string &key, const std::string &encTitle)
{
    std::shared_ptr<std::string> unifiedKey = std::make_shared<std::string>(key);
    Buffer buf(encTitle.size());

    try
    {
        size_t decLen = base64urldecode(encTitle.c_str(), encTitle.size(), buf.buf(), buf.bufSize());
        buf.setDataSize(decLen);

        //Create temporary strongvelope instance to decrypt chat title
        strongvelope::ProtocolHandler *auxCrypto = newStrongvelope(chatId, true, unifiedKey, false, Id::inval());

        auto wptr = getDelTracker();
        promise::Promise<std::string> pms = auxCrypto->decryptChatTitleFromApi(buf);
        return pms.then([wptr, this, chatId, auxCrypto](const std::string title)
        {
            wptr.throwIfDeleted();
            delete auxCrypto;
            return title;
        })
        .fail([wptr, this, chatId, auxCrypto](const promise::Error& err)
        {
            wptr.throwIfDeleted();
            KR_LOG_ERROR("Error decrypting chat title for chat link preview %s:\n%s", ID_CSTR(chatId), err.what());
            delete auxCrypto;
            return err;
        });
    }
    catch(std::exception& e)
    {
        std::string err("Failed to base64-decode chat title for chat ");
        err.append(ID_CSTR(chatId)).append(": ");
        KR_LOG_ERROR("%s", err.c_str());
        return promise::Error(err);
    }
}

promise::Promise<void> Client::setPublicChatToPrivate(karere::Id chatid)
{
    GroupChatRoom *room = (GroupChatRoom *) chats->at(chatid);
    promise::Promise<std::shared_ptr<Buffer>> pms;
    if (room->hasTitle())
    {
        // encrypt chat-topic as in private mode, for same users (0), but creating a random key instead of using unified-key (true)
        pms = room->chat().crypto()->encryptChatTitle(room->titleString(), 0, true);
    }
    else
    {
        pms = promise::Promise<std::shared_ptr<Buffer>>();
        pms.resolve(std::make_shared<Buffer>());
    }

    auto wptr = weakHandle();
    return pms.then([wptr, this, chatid, room](const std::shared_ptr<Buffer>& encTitle)
    {
        wptr.throwIfDeleted();

        std::string auxbuf = base64urlencode(encTitle->buf(), encTitle->dataSize());
        const char *enctitleB64 = encTitle->dataSize() ? auxbuf.c_str() : NULL;

        return api.call(&::mega::MegaApi::chatLinkClose, chatid, enctitleB64)
        .then([this, room, wptr, chatid](ReqResult) -> promise::Promise<void>
        {
            if (wptr.deleted())
                return promise::_Void();

            room->setChatPrivateMode();
            return promise::_Void();
        });
    });
}

promise::Promise<uint64_t> Client::deleteChatLink(karere::Id chatid)
{
    auto wptr = weakHandle();

    return api.call(&::mega::MegaApi::chatLinkDelete, chatid)
    .then([this, wptr, chatid](ReqResult) -> promise::Promise<uint64_t>
    {
        if (wptr.deleted())
            return Id::inval().val;

        return Id::inval().val;
    });
}

promise::Promise<uint64_t> Client::getPublicHandle(Id chatid, bool createifmissing)
{
    auto wptr = weakHandle();

    ApiPromise pms;
    if (createifmissing)
    {
        pms = api.call(&::mega::MegaApi::chatLinkCreate, chatid);
    }
    else
    {
        pms = api.call(&::mega::MegaApi::chatLinkQuery, chatid);
    }

    return pms.then([this, chatid, wptr](ReqResult result) -> promise::Promise<uint64_t>
    {
        if (wptr.deleted())
            return Id::inval().val;

        return result->getParentHandle();
    });
}

void Client::onSyncReceived(Id chatid)
{
    if (mSyncCount <= 0)
    {
        KR_LOG_WARNING("Unexpected SYNC received for chat: %s", ID_CSTR(chatid));
        return;
    }

    mSyncCount--;
    if (mSyncCount == 0 && mSyncTimer)
    {
        cancelTimeout(mSyncTimer, appCtx);
        mSyncTimer = 0;

        mSyncPromise.resolve();
    }
}

bool Client::isChatRoomOpened(Id chatid)
{
    auto it = chats->find(chatid);
    if (it != chats->end())
    {
        return it->second->hasChatHandler();
    }
    return false;
}

promise::Promise<void> Client::loginSdkAndInit(const char* sid)
{
    init(sid);
    if (!sid)
    {
        return sdkLoginNewSession();
    }
    else
    {
        return sdkLoginExistingSession(sid);
    }
}

void Client::saveDb()
{
    try
    {
        if (db.isOpen())
        {
            db.commit();
        }
    }
    catch(std::runtime_error& e)
    {
        KR_LOG_ERROR("Error saving changes to local cache: %s", e.what());
        setInitState(kInitErrCorruptCache);
    }
}

promise::Promise<void> Client::pushReceived(Id chatid)
{
    // if already sent SYNCs or we are not logged in right now...
    if (mSyncTimer || !mChatdClient || !mChatdClient->areAllChatsLoggedIn())
    {
        return mSyncPromise;
        // promise will resolve once logged in for all chats or after receive all SYNCs back
    }

    auto wptr = weakHandle();
    mSyncPromise = Promise<void>();
    mSyncCount = 0;
    mSyncTimer = karere::setTimeout([this, wptr]()
    {
        if (wptr.deleted())
          return;

        assert(mSyncCount != 0);
        mSyncTimer = 0;
        mSyncCount = -1;

        mChatdClient->retryPendingConnections(true);

    }, chatd::kSyncTimeout, appCtx);

    if (chatid.isValid())
    {
        ChatRoom *chat = chats->at(chatid);
        mSyncCount++;
        chat->sendSync();
    }
    else
    {
        for (auto& item: *chats)
        {
            ChatRoom *chat = item.second;
            if (!chat->chat().isDisabled())
            {
                mSyncCount++;
                chat->sendSync();
            }
        }
    }

    return mSyncPromise;
}

void Client::loadContactListFromApi()
{
    std::unique_ptr<::mega::MegaUserList> contacts(api.sdk.getContacts());
    loadContactListFromApi(*contacts);
}

void Client::loadContactListFromApi(::mega::MegaUserList& contacts)
{
#ifndef NDEBUG
    dumpContactList(contacts);
#endif
    contactList->syncWithApi(contacts);
    mContactsLoaded = true;
}

Client::InitState Client::initWithAnonymousSession()
{
    setInitState(kInitAnonymousMode);
    mSid.clear();
    createDb();
    mMyHandle = Id::null(); // anonymous mode should use ownHandle set to all zeros
    mUserAttrCache.reset(new UserAttrCache(*this));
    mChatdClient.reset(new chatd::Client(this));

    return mInitState;
}

promise::Promise<void> Client::initWithNewSession(const char* sid, const std::string& scsn,
    const std::shared_ptr<::mega::MegaUserList>& contactList,
    const std::shared_ptr<::mega::MegaTextChatList>& chatList)
{
    assert(sid);

    mSid = sid;
    createDb();

// We have a complete snapshot of the SDK contact and chat list state.
// Commit it with the accompanying scsn
    mMyHandle = getMyHandleFromSdk();
    db.query("insert or replace into vars(name,value) values('my_handle', ?)", mMyHandle);

    mMyEmail = getMyEmailFromSdk();
    db.query("insert or replace into vars(name,value) values('my_email', ?)", mMyEmail);

    mMyIdentity = (static_cast<uint64_t>(rand()) << 32) | time(NULL);
    db.query("insert or replace into vars(name,value) values('clientid_seed', ?)", mMyIdentity);

    mUserAttrCache.reset(new UserAttrCache(*this));
    api.sdk.addGlobalListener(this);

    auto wptr = weakHandle();
    return loadOwnKeysFromApi()
    .then([this, scsn, contactList, chatList, wptr]()
    {
        if (wptr.deleted())
            return;
        loadContactListFromApi(*contactList);
        mChatdClient.reset(new chatd::Client(this));
        assert(chats->empty());
        chats->onChatsUpdate(*chatList);
        commit(scsn);
    });
}

void Client::setCommitEach(bool commitEach)
{
    db.setCommitEach(commitEach);
}

void Client::commit(const std::string& scsn)
{
    if (scsn.empty())
    {
        KR_LOG_DEBUG("Committing with empty scsn");
        db.commit();
        return;
    }
    if (scsn == mLastScsn)
    {
        KR_LOG_DEBUG("Committing with same scsn");
        db.commit();
        return;
    }

    db.query("insert or replace into vars(name,value) values('scsn',?)", scsn);
    db.commit();
    mLastScsn = scsn;
    KR_LOG_DEBUG("Commit with scsn %s", scsn.c_str());
}

void Client::onEvent(::mega::MegaApi* /*api*/, ::mega::MegaEvent* event)
{
    assert(event);
    int type = event->getType();
    switch (type)
    {
    case ::mega::MegaEvent::EVENT_COMMIT_DB:
    {
        const char *pscsn = event->getText();
        if (!pscsn)
        {
            KR_LOG_ERROR("EVENT_COMMIT_DB --> DB commit triggered by SDK without a valid scsn");
            return;
        }

        std::string scsn = pscsn;
        auto wptr = weakHandle();
        marshallCall([wptr, this, scsn]()
        {
            if (wptr.deleted())
            {
                return;
            }

            if (db.isOpen())
            {
                KR_LOG_DEBUG("EVENT_COMMIT_DB --> DB commit triggered by SDK");
                commit(scsn);
            }

        }, appCtx);
        break;
    }

    default:
        break;
    }
}

void Client::initWithDbSession(const char* sid)
{
    try
    {
        assert(sid);
        if (!openDb(sid))
        {
            assert(mSid.empty());
            setInitState(kInitErrNoCache);
            return;
        }
        assert(db);
        assert(!mSid.empty());
        mUserAttrCache.reset(new UserAttrCache(*this));
        api.sdk.addGlobalListener(this);

        mMyHandle = getMyHandleFromDb();
        assert(mMyHandle && mMyHandle.isValid());

        mMyEmail = getMyEmailFromDb();

        mMyIdentity = getMyIdentityFromDb();

        mOwnNameAttrHandle = mUserAttrCache->getAttr(mMyHandle, USER_ATTR_FULLNAME, this,
        [](Buffer* buf, void* userp)
        {
            if (!buf || buf->empty())
                return;
            auto& name = static_cast<Client*>(userp)->mMyName;
            name.assign(buf->buf(), buf->dataSize());
        });

        loadOwnKeysFromDb();
        contactList->loadFromDb();
        mContactsLoaded = true;
        mChatdClient.reset(new chatd::Client(this));
        chats->loadFromDb();
    }
    catch(std::runtime_error& e)
    {
        KR_LOG_ERROR("initWithDbSession: Error loading session from local cache: %s", e.what());
        setInitState(kInitErrCorruptCache);
        return;
    }

    setInitState(kInitHasOfflineSession);
    return;
}

void Client::setInitState(InitState newState)
{
    if (newState == mInitState)
        return;
    mInitState = newState;
    KR_LOG_DEBUG("Client reached init state %s", initStateStr());
    app.onInitStateChange(mInitState);
}

Client::InitState Client::init(const char* sid)
{
    if (mInitState > kInitCreated)
    {
        KR_LOG_ERROR("init: karere is already initialized. Current state: %s", initStateStr());
        return kInitErrAlready;
    }

    if (sid)
    {
        initWithDbSession(sid);
        if (mInitState == kInitErrNoCache ||    // not found, uncompatible db version, cannot open
                mInitState == kInitErrCorruptCache)
        {
            wipeDb(sid);
        }
    }
    else
    {
        setInitState(kInitWaitingNewSession);
    }
    api.sdk.addRequestListener(this);
    return mInitState;
}

void Client::onRequestFinish(::mega::MegaApi* /*apiObj*/, ::mega::MegaRequest *request, ::mega::MegaError* e)
{
    int reqType = request->getType();
    int errorCode = e->getErrorCode();
    if (errorCode != ::mega::MegaError::API_OK && reqType != ::mega::MegaRequest::TYPE_LOGOUT)
    {
        KR_LOG_ERROR("Request %s finished with error %s", request->getRequestString(), e->getErrorString());
        return;
    }

    switch (reqType)
    {
    case ::mega::MegaRequest::TYPE_LOGOUT:
    {
        bool loggedOut = ((errorCode == ::mega::MegaError::API_OK || errorCode == ::mega::MegaError::API_ESID)
                          && request->getFlag());    // SDK has been logged out normally closing session

        bool sessionExpired = request->getParamType() == ::mega::MegaError::API_ESID;       // SDK received ESID during login or any other request
        if (loggedOut)
            KR_LOG_DEBUG("Logout detected in the SDK. Closing MEGAchat session...");

        if (sessionExpired)
            KR_LOG_WARNING("Expired session detected. Closing MEGAchat session...");

        if (loggedOut || sessionExpired)
        {
            auto wptr = weakHandle();
            marshallCall([wptr, this]() // update state in the karere thread
            {
                if (wptr.deleted())
                    return;

                if (!isTerminated())
                {
                    setInitState(kInitErrSidInvalid);
                }
            }, appCtx);
            return;
        }
        break;
    }

    case ::mega::MegaRequest::TYPE_FETCH_NODES:
    {
        api.sdk.pauseActionPackets();
        auto state = mInitState;
        char* pscsn = api.sdk.getSequenceNumber();
        std::string scsn;
        if (pscsn)
        {
            scsn = pscsn;
            delete[] pscsn;
        }
        std::shared_ptr<::mega::MegaUserList> contactList(api.sdk.getContacts());
        std::shared_ptr<::mega::MegaTextChatList> chatList(api.sdk.getChatList());

        auto wptr = weakHandle();
        marshallCall([wptr, this, state, scsn, contactList, chatList]()
        {
            if (wptr.deleted())
                return;

            if (state == kInitHasOfflineSession)
            {
// disable this safety checkup, since dumpSession() differs from first-time login value
//              std::unique_ptr<char[]> sid(api.sdk.dumpSession());
//              assert(sid);
//              // we loaded our state from db
//              // verify the SDK sid is the same as ours
//              if (mSid != sid.get())
//              {
//                  setInitState(kInitErrSidMismatch);
//                  return;
//              }
                checkSyncWithSdkDb(scsn, *contactList, *chatList);
                setInitState(kInitHasOnlineSession);
                mSessionReadyPromise.resolve();
                api.sdk.resumeActionPackets();
            }
            else if (state == kInitWaitingNewSession || state == kInitErrNoCache)
            {
                std::unique_ptr<char[]> sid(api.sdk.dumpSession());
                assert(sid);
                initWithNewSession(sid.get(), scsn, contactList, chatList)
                .fail([this](const promise::Error& err)
                {
                    mSessionReadyPromise.reject(err);
                    api.sdk.resumeActionPackets();
                    return err;
                })
                .then([this]()
                {
                    setInitState(kInitHasOnlineSession);
                    mSessionReadyPromise.resolve();
                    api.sdk.resumeActionPackets();
                });
            }
            else
            {
                assert(state == kInitHasOnlineSession);
                api.sdk.resumeActionPackets();
            }
        }, appCtx);
        break;
    }

    case ::mega::MegaRequest::TYPE_SET_ATTR_USER:
    {
        int attrType = request->getParamType();
        int changeType;
        if (attrType == ::mega::MegaApi::USER_ATTR_FIRSTNAME)
        {
            changeType = ::mega::MegaUser::CHANGE_TYPE_FIRSTNAME;
        }
        else if (attrType == ::mega::MegaApi::USER_ATTR_LASTNAME)
        {
            changeType = ::mega::MegaUser::CHANGE_TYPE_LASTNAME;
        }
        else
        {
            return;
        }

        auto wptr = weakHandle();
        marshallCall([wptr, this, changeType]()
        {
            if (wptr.deleted())
                return;

            mUserAttrCache->onUserAttrChange(mMyHandle, changeType);
        }, appCtx);
        break;
    }

    default:    // no action to be taken for other type of requests
    {
        break;
    }
    }
}

//TODO: We should actually wipe the whole app dir, but the log file may
//be in that dir, and it is in use
void Client::wipeDb(const std::string& sid)
{
    db.close();
    std::string path = dbPath(sid);
    remove(path.c_str());
    struct stat info;
    if (stat(path.c_str(), &info) == 0)
        throw std::runtime_error("wipeDb: Could not delete old database file in "+mAppDir);
}

void Client::createDb()
{
    wipeDb(mSid);
    std::string path = dbPath(mSid);
    if (!db.open(path.c_str(), false))
        throw std::runtime_error("Can't access application database at "+mAppDir);
    createDbSchema(); //calls commit() at the end
}

bool Client::checkSyncWithSdkDb(const std::string& scsn,
    ::mega::MegaUserList& contactList, ::mega::MegaTextChatList& chatList)
{
    SqliteStmt stmt(db, "select value from vars where name='scsn'");
    stmt.stepMustHaveData("get karere scsn");
    if (stmt.stringCol(0) == scsn)
    {
        KR_LOG_DEBUG("Db sync ok, karere scsn matches with the one from sdk");
        return true;
    }

    // We are not in sync, probably karere is one or more commits behind
    KR_LOG_WARNING("Karere db out of sync with sdk - scsn-s don't match. Will reload all state from SDK");

    // invalidate user attrib cache
    mUserAttrCache->invalidate();
    // sync contactlist first
    loadContactListFromApi(contactList);
    // sync the chatroom list
    chats->onChatsUpdate(chatList);
    // commit the snapshot
    commit(scsn);
    return false;
}

void Client::dumpChatrooms(::mega::MegaTextChatList& chatRooms)
{
    KR_LOG_DEBUG("=== Chatrooms received from API: ===");
    for (int i=0; i<chatRooms.size(); i++)
    {
        auto& room = *chatRooms.get(i);
        if (room.isGroup())
        {
            KR_LOG_DEBUG("%s(group, ownPriv=%s):",
                ID_CSTR(room.getHandle()), privToString((chatd::Priv)room.getOwnPrivilege()));
        }
        else
        {
            KR_LOG_DEBUG("%s(1on1)", ID_CSTR(room.getHandle()));
        }
        auto peers = room.getPeerList();
        if (!peers)
        {
            KR_LOG_DEBUG("  (room has no peers)");
            continue;
        }
        for (int j = 0; j<peers->size(); j++)
            KR_LOG_DEBUG("  %s: %s", ID_CSTR(peers->getPeerHandle(j)),
                privToString((chatd::Priv)peers->getPeerPrivilege(j)));
    }
    KR_LOG_DEBUG("=== Chatroom list end ===");
}
void Client::dumpContactList(::mega::MegaUserList& clist)
{
    KR_LOG_DEBUG("== Contactlist received from API: ==");
    for (int i=0; i< clist.size(); i++)
    {
        auto& user = *clist.get(i);
        auto visibility = user.getVisibility();
        if (visibility != ::mega::MegaUser::VISIBILITY_VISIBLE)
            KR_LOG_DEBUG("  %s (visibility = %d)", ID_CSTR(user.getHandle()), visibility);
        else
            KR_LOG_DEBUG("  %s", ID_CSTR(user.getHandle()));
    }
    KR_LOG_DEBUG("== Contactlist end ==");
}

promise::Promise<void> Client::connect(Presence pres, bool isInBackground)
{
// only the first connect() needs to wait for the mSessionReadyPromise.
// Any subsequent connect()-s (preceded by disconnect()) can initiate
// the connect immediately
    if (mConnState == kConnecting)      // already connecting, wait for completion
    {
        return mConnectPromise;
    }
    else if (mConnState == kConnected)  // nothing to do
    {
        return promise::_Void();
    }

    assert(mConnState == kDisconnected);

    if (anonymousMode())
    {
        return doConnect(pres, isInBackground);
    }

    auto sessDone = mSessionReadyPromise.done();    // wait for fetchnodes completion
    switch (sessDone)
    {
        case promise::kSucceeded:   // if session is ready...
            return doConnect(pres, isInBackground);

        case promise::kFailed:      // if session failed...
            return mSessionReadyPromise.error();

        default:                    // if session is not ready yet... wait for it and then connect
            assert(sessDone == promise::kNotResolved);
            mConnectPromise = mSessionReadyPromise
            .then([this, pres, isInBackground]() mutable
            {
                return doConnect(pres, isInBackground);
            });
            return mConnectPromise;
    }
}

promise::Promise<void> Client::doConnect(Presence pres, bool isInBackground)
{
    KR_LOG_DEBUG("Connecting to account '%s'(%s)...", SdkString(api.sdk.getMyEmail()).c_str(), mMyHandle.toString().c_str());

    setConnState(kConnecting);

    connectToChatd(isInBackground);

    // start heartbeats timer
    assert(!mHeartbeatTimer);
    auto wptr = weakHandle();
    mHeartbeatTimer = karere::setInterval([this, wptr]()
    {
        if (wptr.deleted() || !mHeartbeatTimer)
        {
            return;
        }

        heartbeat();
    }, kHeartbeatTimeout, appCtx);

    // notify user-attr cache
    assert(mUserAttrCache);
    mUserAttrCache->onLogin();

    if (anonymousMode())
    {
        // avoid to connect to presenced (no user, no peerstatus)
        // avoid to retrieve own user-attributes (no user, no attributes)
        // avoid to initialize WebRTC (no user, no calls)
        setConnState(kConnected);
        return ::promise::_Void();
    }

    mOwnPresence = pres;
    assert(mSessionReadyPromise.succeeded());

    mOwnNameAttrHandle = mUserAttrCache->getAttr(mMyHandle, USER_ATTR_FULLNAME, this,
    [](Buffer* buf, void* userp)
    {
        if (!buf || buf->empty())
            return;
        auto& name = static_cast<Client*>(userp)->mMyName;
        name.assign(buf->buf(), buf->dataSize());
        KR_LOG_DEBUG("Own screen name is: '%s'", name.c_str()+1);
    });

#ifndef KARERE_DISABLE_WEBRTC
    // Create the rtc module
    rtc.reset(rtcModule::create(*this, *this, new rtcModule::RtcCrypto(*this), KARERE_DEFAULT_TURN_SERVERS));
    rtc->init();
#endif

    auto pms = connectToPresenced(mOwnPresence)
    .then([this, wptr]()
    {
        if (wptr.deleted())
        {
            return;
        }

        setConnState(kConnected);
    })
    .fail([this](const promise::Error& err)
    {
        setConnState(kDisconnected);
        return err;
    });

    return pms;
}

void Client::setConnState(ConnState newState)
{
    mConnState = newState;
    KR_LOG_DEBUG("Client connection state changed to %s", connStateToStr(newState));
}
karere::Id Client::getMyHandleFromSdk()
{
    SdkString uh = api.sdk.getMyUserHandle();
    if (!uh.c_str() || !uh.c_str()[0])
        throw std::runtime_error("Could not get our own user handle from API");
    KR_LOG_INFO("Our user handle is %s", uh.c_str());
    karere::Id result(uh.c_str());
    if (result == Id::null() || result.val == ::mega::UNDEF)
        throw std::runtime_error("Own handle returned by the SDK is NULL");
    return result;
}

std::string Client::getMyEmailFromDb()
{
    SqliteStmt stmt(db, "select value from vars where name='my_email'");
    if (!stmt.step())
        throw std::runtime_error("No own email in database");

    std::string email = stmt.stringCol(0);

    if (email.length() < 5)
        throw std::runtime_error("loadOwnEmailFromDb: Own email in db is invalid");
    return email;
}

std::string Client::getMyEmailFromSdk()
{
    SdkString myEmail = api.sdk.getMyEmail();
    if (!myEmail.c_str() || !myEmail.c_str()[0])
        throw std::runtime_error("Could not get our own email from API");
    KR_LOG_INFO("Our email address is %s", myEmail.c_str());
    return myEmail.c_str();
}

karere::Id Client::getMyHandleFromDb()
{
    SqliteStmt stmt(db, "select value from vars where name='my_handle'");
    if (!stmt.step())
        throw std::runtime_error("No own user handle in database");

    karere::Id result = stmt.uint64Col(0);

    if (result == Id::null() || result.val == mega::UNDEF)
        throw std::runtime_error("loadOwnUserHandleFromDb: Own handle in db is invalid");
    return result;
}

uint64_t Client::getMyIdentityFromDb()
{
    uint64_t result = 0;

    SqliteStmt stmt(db, "select value from vars where name='clientid_seed'");
    if (!stmt.step())
    {
        KR_LOG_WARNING("clientid_seed not found in DB. Creating a new one");
        result = (static_cast<uint64_t>(rand()) << 32) | time(NULL);
        db.query("insert or replace into vars(name,value) values('clientid_seed', ?)", result);
    }
    else
    {
        result = stmt.uint64Col(0);
        if (result == 0)
        {
            KR_LOG_WARNING("clientid_seed in DB is invalid. Creating a new one");
            result = (static_cast<uint64_t>(rand()) << 32) | time(NULL);
            db.query("insert or replace into vars(name,value) values('clientid_seed', ?)", result);
        }
    }
    return result;
}

promise::Promise<void> Client::loadOwnKeysFromApi()
{
    return api.call(&::mega::MegaApi::getUserAttribute, (int)mega::MegaApi::USER_ATTR_KEYRING)
    .then([this](ReqResult result) -> ApiPromise
    {
        auto keys = result->getMegaStringMap();
        auto cu25519 = keys->get("prCu255");
        if (!cu25519)
            return promise::Error("prCu255 private key missing in keyring from API");
        auto ed25519 = keys->get("prEd255");
        if (!ed25519)
            return promise::Error("prEd255 private key missing in keyring from API");

        auto b64len = strlen(cu25519);
        if (b64len != 43)
            return promise::Error("prCu255 base64 key length is not 43 bytes");
        base64urldecode(cu25519, b64len, mMyPrivCu25519, sizeof(mMyPrivCu25519));

        b64len = strlen(ed25519);
        if (b64len != 43)
            return promise::Error("prEd255 base64 key length is not 43 bytes");
        base64urldecode(ed25519, b64len, mMyPrivEd25519, sizeof(mMyPrivEd25519));
        return api.call(&mega::MegaApi::getUserData);
    })
    .then([this](ReqResult result) -> promise::Promise<void>
    {
        auto pubrsa = result->getPassword();
        if (!pubrsa)
            return promise::Error("No public RSA key in getUserData API response");
        mMyPubRsaLen = base64urldecode(pubrsa, strlen(pubrsa), mMyPubRsa, sizeof(mMyPubRsa));
        auto privrsa = result->getPrivateKey();
        if (!privrsa)
            return promise::Error("No private RSA key in getUserData API response");
        mMyPrivRsaLen = base64urldecode(privrsa, strlen(privrsa), mMyPrivRsa, sizeof(mMyPrivRsa));
        // write to db
        db.query("insert or replace into vars(name, value) values('pr_cu25519', ?)", StaticBuffer(mMyPrivCu25519, sizeof(mMyPrivCu25519)));
        db.query("insert or replace into vars(name, value) values('pr_ed25519', ?)", StaticBuffer(mMyPrivEd25519, sizeof(mMyPrivEd25519)));
        db.query("insert or replace into vars(name, value) values('pub_rsa', ?)", StaticBuffer(mMyPubRsa, mMyPubRsaLen));
        db.query("insert or replace into vars(name, value) values('pr_rsa', ?)", StaticBuffer(mMyPrivRsa, mMyPrivRsaLen));
        KR_LOG_DEBUG("loadOwnKeysFromApi: success");
        return promise::_Void();
    });
}

void Client::loadOwnKeysFromDb()
{
    SqliteStmt stmt(db, "select value from vars where name=?");

    stmt << "pr_rsa";
    stmt.stepMustHaveData();
    mMyPrivRsaLen = stmt.blobCol(0, mMyPrivRsa, sizeof(mMyPrivRsa));
    stmt.reset().clearBind();
    stmt << "pub_rsa";
    stmt.stepMustHaveData();
    mMyPubRsaLen = stmt.blobCol(0, mMyPubRsa, sizeof(mMyPubRsa));

    stmt.reset().clearBind();
    stmt << "pr_cu25519";
    stmt.stepMustHaveData();
    auto len = stmt.blobCol(0, mMyPrivCu25519, sizeof(mMyPrivCu25519));
    if (len != sizeof(mMyPrivCu25519))
        throw std::runtime_error("Unexpected length of privCu25519 in database");
    stmt.reset().clearBind();
    stmt << "pr_ed25519";
    stmt.stepMustHaveData();
    len = stmt.blobCol(0, mMyPrivEd25519, sizeof(mMyPrivEd25519));
    if (len != sizeof(mMyPrivEd25519))
        throw std::runtime_error("Unexpected length of privEd2519 in database");
}


promise::Promise<void> Client::connectToPresenced(Presence forcedPres)
{
    if (mPresencedUrl.empty())
    {
        return api.call(&::mega::MegaApi::getChatPresenceURL)
        .then([this, forcedPres](ReqResult result) -> Promise<void>
        {
            auto url = result->getLink();
            if (!url)
                return promise::Error("No presenced URL received from API");
            mPresencedUrl = url;
            return connectToPresencedWithUrl(mPresencedUrl, forcedPres);
        });
    }
    else
    {
        return connectToPresencedWithUrl(mPresencedUrl, forcedPres);
    }
}

promise::Promise<void> Client::connectToPresencedWithUrl(const std::string& url, Presence pres)
{
//we assume app.onOwnPresence(Presence::kOffline) has been called at application start

    // Prepare list of peers to subscribe to its presence
    // add contacts
    presenced::IdRefMap peers;
    for (auto& contact: *contactList)
    {
        if (contact.second->visibility() == ::mega::MegaUser::VISIBILITY_VISIBLE)
        {
            peers.insert(contact.first);
        }
    }
    // add peers from groupchats
    for (auto& chat: *chats)
    {
        if (!chat.second->isGroup())
        {
            continue;
        }

        auto& members = static_cast<GroupChatRoom*>(chat.second)->peers();
        for (auto& peer: members)
        {
            if (!contactList->isExContact(peer.first))
            {
                peers.insert(peer.first);
            }
        }
    }

    // Notify presence, if any
    if (pres.isValid())
    {
        mOwnPresence = pres;
        app.onPresenceChanged(mMyHandle, pres, true);
    }

    return mPresencedClient.connect(url, std::move(peers), presenced::Config(pres));
}

void Contact::updatePresence(Presence pres)
{
    mPresence = pres;
    updateAllOnlineDisplays(pres);
}
// presenced handlers
void Client::onPresenceChange(Id userid, Presence pres)
{
    if (isTerminated())
    {
        return;
    }

    if (userid == mMyHandle)
    {
        mOwnPresence = pres;
    }
    else
    {
        contactList->onPresenceChanged(userid, pres);
    }
    for (auto& item: *chats)
    {
        auto& chat = *item.second;
        if (!chat.isGroup())
            continue;
        static_cast<GroupChatRoom&>(chat).updatePeerPresence(userid, pres);
    }
    app.onPresenceChanged(userid, pres, false);
}
void Client::onPresenceConfigChanged(const presenced::Config& state, bool pending)
{
    app.onPresenceConfigChanged(state, pending);
}

void Client::onPresenceLastGreenUpdated(Id userid, uint16_t lastGreen)
{
    app.onPresenceLastGreenUpdated(userid, lastGreen);
}

void Client::onConnStateChange(presenced::Client::ConnState /*state*/)
{

}

void GroupChatRoom::updatePeerPresence(uint64_t userid, Presence pres)
{
    auto it = mPeers.find(userid);
    if (it == mPeers.end())
        return;
    it->second->mPresence = pres;
}

void Client::notifyNetworkOffline()
{
}
void Client::notifyNetworkOnline()
{
}
void Client::notifyUserIdle()
{
    if (mChatdClient)
    {
        mChatdClient->notifyUserIdle();
    }
}
void Client::notifyUserActive()
{
    if (mChatdClient)
    {
        mChatdClient->notifyUserActive();
    }
}

void Client::terminate(bool deleteDb)
{
    setInitState(kInitTerminated);

    api.sdk.removeRequestListener(this);
    api.sdk.removeGlobalListener(this);

#ifndef KARERE_DISABLE_WEBRTC
    if (rtc)
        rtc->hangupAll(rtcModule::TermCode::kAppTerminating);
#endif

    // pre-destroy chatrooms in preview mode (cleanup from DB + send HANDLELEAVE)
    // Otherwise, DB will be already closed when the GroupChatRoom dtor is called
    for (auto it = chats->begin(); it != chats->end();)
    {
        if (it->second->previewMode())
        {
            delete it->second;
            auto itToRemove = it;
            it++;
            chats->erase(itToRemove);
        }
        else
        {
            it++;
        }
    }
  
    if (mConnState != kDisconnected)
    {
        setConnState(kDisconnected);

        // stop syncing own-name and close user-attributes cache
        mUserAttrCache->removeCb(mOwnNameAttrHandle);
        mUserAttrCache->onLogOut();
        mUserAttrCache.reset();

        // stop heartbeats
        if (mHeartbeatTimer)
        {
            karere::cancelInterval(mHeartbeatTimer, appCtx);
            mHeartbeatTimer = 0;
        }

        // disconnect from chatd shards and presenced
        mChatdClient->disconnect();
        mPresencedClient.disconnect();
    }

    // close or delete MEGAchat's DB file
    try
    {
        if (deleteDb)
        {
            wipeDb(mSid);
        }
        else if (db.isOpen())
        {
            KR_LOG_INFO("Doing final COMMIT to database");
            db.commit();
            db.close();
        }
    }
    catch(std::runtime_error& e)
    {
        KR_LOG_ERROR("Error saving changes to local cache during termination: %s", e.what());
    }
}

promise::Promise<void> Client::setPresence(Presence pres)
{
    if (pres == mPresencedClient.config().presence())
    {
        std::string err = "setPresence: tried to change online state to the current configured state (";
        err.append(mOwnPresence.toString(mOwnPresence)).append(")");
        return promise::Error(err, kErrorArgs);
    }

    bool ret = mPresencedClient.setPresence(pres);
    if (!ret)
    {
        return promise::Error("setPresence: not connected", kErrorAccess);
    }

    app.onPresenceChanged(mMyHandle, pres, true);
    return promise::_Void();
}

void Client::onUsersUpdate(mega::MegaApi* /*api*/, mega::MegaUserList *aUsers)
{
    if (!aUsers)
        return;

    std::shared_ptr<mega::MegaUserList> users(aUsers->copy());
    auto wptr = weakHandle();
    marshallCall([wptr, this, users]()
    {
        if (wptr.deleted())
        {
            return;
        }

        assert(mUserAttrCache);
        auto count = users->size();
        for (int i = 0; i < count; i++)
        {
            auto& user = *users->get(i);
            if (user.getChanges())
            {
                if (user.isOwnChange() == 0)
                {
                    mUserAttrCache->onUserAttrChange(user);
                }
            }
            else
            {
                contactList->onUserAddRemove(user);
            }
        };
    }, appCtx);
}

promise::Promise<karere::Id>
Client::createGroupChat(std::vector<std::pair<uint64_t, chatd::Priv>> peers, bool publicchat, const char *title)
{
    // prepare set of participants
    std::shared_ptr<mega::MegaTextChatPeerList> sdkPeers(mega::MegaTextChatPeerList::createInstance());
    std::shared_ptr<SetOfIds> users = std::make_shared<SetOfIds>();
    users->insert(mMyHandle);
    for (auto& peer: peers)
    {
        sdkPeers->addPeer(peer.first, peer.second);
        users->insert(peer.first);
    }

    // prepare unified key (if public chat)
    std::shared_ptr<std::string> unifiedKey;
    if (publicchat)
    {
        Buffer *buf = strongvelope::ProtocolHandler::createUnifiedKey();
        unifiedKey.reset(new std::string(buf->buf(), buf->dataSize()));
        delete buf;
    }

    // create strongvelope for encryption of title/unified-key
    std::shared_ptr<strongvelope::ProtocolHandler> crypto;
    if (publicchat || title)
    {
        crypto.reset(new strongvelope::ProtocolHandler(mMyHandle,
                StaticBuffer(mMyPrivCu25519, 32), StaticBuffer(mMyPrivEd25519, 32),
                StaticBuffer(mMyPrivRsa, mMyPrivRsaLen), *mUserAttrCache, db, karere::Id::inval(), publicchat,
                unifiedKey, false, Id::inval(), appCtx));
        crypto->setUsers(users.get());  // ownership belongs to this method, it will be released after `crypto`
    }

    promise::Promise<std::shared_ptr<Buffer>> pms;
    if (title)
    {
        const std::string auxTitle(title);
        pms = crypto->encryptChatTitle(auxTitle);
    }
    else
    {
        pms = promise::Promise<std::shared_ptr<Buffer>>();
        pms.resolve(std::make_shared<Buffer>());
    }

    // capture `users`, since it's used at strongvelope for encryption of unified-key in public chats
    auto wptr = getDelTracker();
    return pms.then([wptr, this, crypto, users, sdkPeers, publicchat](const std::shared_ptr<Buffer>& encTitle)
    {
        std::string enctitleB64;
        if (!encTitle->empty())
        {
            enctitleB64 = base64urlencode(encTitle->buf(), encTitle->dataSize());
        }

        ApiPromise createChatPromise;

        if (publicchat)
        {
            createChatPromise = crypto->encryptUnifiedKeyForAllParticipants()
            .then([wptr, this, sdkPeers, enctitleB64](chatd::KeyCommand *keyCmd) -> ApiPromise
            {
                mega::MegaStringMap *userKeyMap;
                userKeyMap = mega::MegaStringMap::createInstance();

                for (int i = 0; i < sdkPeers->size(); i++)
                {
                    //Get peer Handle in B64
                    Id peerHandle(sdkPeers->getPeerHandle(i));

                    //Get peer unified key
                    auto useruk = keyCmd->getKeyByUserId(peerHandle);

                    //Append [creatorhandle+uk]
                    std::string uKeyBin((const char*)&mMyHandle, sizeof(mMyHandle.val));
                    uKeyBin.append(useruk->buf(), useruk->size());

                    //Encode [creatorhandle+uk] to B64
                    std::string uKeyB64;
                    mega::Base64::btoa(uKeyBin, uKeyB64);

                    //Add entry to map
                    userKeyMap->set(peerHandle.toString().c_str(), uKeyB64.c_str());
                }

                //Get own unified key
                auto ownKey = keyCmd->getKeyByUserId(mMyHandle);

                //Append [creatorhandle+uk]
                std::string okeyBin((const char*)&mMyHandle, sizeof(mMyHandle.val));
                okeyBin.append(ownKey->buf(), ownKey->size());

                //Encode [creatorhandle+uk] to B64
                std::string oKeyB64;
                mega::Base64::btoa(okeyBin, oKeyB64);

                //Add entry to map
                userKeyMap->set(mMyHandle.toString().c_str(), oKeyB64.c_str());
                return api.call(&mega::MegaApi::createPublicChat, sdkPeers.get(), userKeyMap,
                                !enctitleB64.empty() ? enctitleB64.c_str() : nullptr);
            });
        }
        else
        {
            createChatPromise = api.call(&mega::MegaApi::createChat, true, sdkPeers.get(),
                                         !enctitleB64.empty() ? enctitleB64.c_str() : nullptr);
        }

        return createChatPromise
        .then([this, wptr](ReqResult result) -> Promise<karere::Id>
        {
            if (wptr.deleted())
                return promise::Error("Chat created successfully, but instance was removed");

            auto& list = *result->getMegaTextChatList();
            if (list.size() != 1)
                return promise::Error("Empty chat list returned from API");

            auto room = chats->addRoom(*list.get(0));
            if (!room || !room->isGroup())
                return promise::Error("API created incorrect group");

            room->connect();
            return karere::Id(room->chatid());
        });
     });
}

promise::Promise<void> GroupChatRoom::excludeMember(uint64_t userid)
{
    auto wptr = getDelTracker();
    return parent.mKarereClient.api.callIgnoreResult(&mega::MegaApi::removeFromChat, chatid(), userid)
    .then([this, wptr, userid]()
    {
        wptr.throwIfDeleted();
        if (removeMember(userid) && !mHasTitle)
        {
            makeTitleFromMemberNames();
        }
    });
}

ChatRoom::ChatRoom(ChatRoomList& aParent, const uint64_t& chatid, bool aIsGroup,
  unsigned char aShard, chatd::Priv aOwnPriv, int64_t ts, bool aIsArchived, const std::string& aTitle)
   :parent(aParent), mChatid(chatid),
    mShardNo(aShard), mIsGroup(aIsGroup),
    mOwnPriv(aOwnPriv), mCreationTs(ts), mIsArchived(aIsArchived), mTitleString(aTitle), mHasTitle(false)
{}

//chatd::Listener
void ChatRoom::onLastMessageTsUpdated(uint32_t ts)
{
    callAfterInit(this, [this, ts]()
    {
        auto display = roomGui();
        if (display)
            display->onLastTsUpdated(ts);
    }, parent.mKarereClient.appCtx);
}

ApiPromise ChatRoom::requestGrantAccess(mega::MegaNode *node, mega::MegaHandle userHandle)
{
    return parent.mKarereClient.api.call(&::mega::MegaApi::grantAccessInChat, chatid(), node, userHandle);
}

ApiPromise ChatRoom::requestRevokeAccess(mega::MegaNode *node, mega::MegaHandle userHandle)
{
    return parent.mKarereClient.api.call(&::mega::MegaApi::removeAccessInChat, chatid(), node, userHandle);
}

strongvelope::ProtocolHandler* Client::newStrongvelope(karere::Id chatid, bool isPublic,
        std::shared_ptr<std::string> unifiedKey, int isUnifiedKeyEncrypted, karere::Id ph)
{
    return new strongvelope::ProtocolHandler(mMyHandle,
         StaticBuffer(mMyPrivCu25519, 32), StaticBuffer(mMyPrivEd25519, 32),
         StaticBuffer(mMyPrivRsa, mMyPrivRsaLen), *mUserAttrCache, db, chatid,
         isPublic, unifiedKey, isUnifiedKeyEncrypted, ph, appCtx);
}

void ChatRoom::createChatdChat(const karere::SetOfIds& initialUsers, bool isPublic,
        std::shared_ptr<std::string> unifiedKey, int isUnifiedKeyEncrypted, const karere::Id ph)
{
    mChat = &parent.mKarereClient.mChatdClient->createChat(
        mChatid, mShardNo, mUrl, this, initialUsers,
        parent.mKarereClient.newStrongvelope(mChatid, isPublic, unifiedKey, isUnifiedKeyEncrypted, ph), mCreationTs, mIsGroup);
}

template <class T, typename F>
void callAfterInit(T* self, F&& func, void *ctx)
{
    if (self->isInitializing())
    {
        auto wptr = self->weakHandle();
        marshallCall([wptr, func]()
        {
            if (!wptr.deleted())
                func();
        }, ctx);
    }
    else
    {
        func();
    }
}

void PeerChatRoom::initWithChatd()
{
    createChatdChat(SetOfIds({Id(mPeer), parent.mKarereClient.myHandle()}));
}

void PeerChatRoom::connect(const char */*url*/)
{
    mChat->connect();
}

#ifndef KARERE_DISABLE_WEBRTC
rtcModule::ICall& ChatRoom::mediaCall(AvFlags av, rtcModule::ICallHandler& handler)
{
    return parent.mKarereClient.rtc->startCall(chatid(), av, handler);
}
#endif

promise::Promise<void> PeerChatRoom::requesGrantAccessToNodes(mega::MegaNodeList *nodes)
{
    std::vector<ApiPromise> promises;

    for (int i = 0; i < nodes->size(); ++i)
    {
        if (!parent.mKarereClient.api.sdk.hasAccessToAttachment(mChatid, nodes->get(i)->getHandle(), peer()))
        {
            ApiPromise promise = requestGrantAccess(nodes->get(i), peer());
            promises.push_back(promise);
        }
    }

    return promise::when(promises);
}

promise::Promise<void> PeerChatRoom::requestRevokeAccessToNode(mega::MegaNode *node)
{
    std::vector<ApiPromise> promises;

    mega::MegaHandleList *megaHandleList = parent.mKarereClient.api.sdk.getAttachmentAccess(mChatid, node->getHandle());

    for (unsigned int j = 0; j < megaHandleList->size(); ++j)
    {
        ApiPromise promise = requestRevokeAccess(node, peer());
        promises.push_back(promise);
    }

    delete megaHandleList;

    return promise::when(promises);
}

promise::Promise<void> GroupChatRoom::requesGrantAccessToNodes(mega::MegaNodeList *nodes)
{
    std::vector<ApiPromise> promises;

    for (int i = 0; i < nodes->size(); ++i)
    {
        if (publicChat())
        {
           if (!parent.mKarereClient.api.sdk.hasAccessToAttachment(mChatid, nodes->get(i)->getHandle(), Id::COMMANDER()))
           {
               ApiPromise promise = requestGrantAccess(nodes->get(i), Id::COMMANDER());
               promises.push_back(promise);
           }
        }
        else
        {
            for (auto it = mPeers.begin(); it != mPeers.end(); ++it)
            {
                if (!parent.mKarereClient.api.sdk.hasAccessToAttachment(mChatid, nodes->get(i)->getHandle(), it->second->mHandle))
                {
                    ApiPromise promise = requestGrantAccess(nodes->get(i), it->second->mHandle);
                    promises.push_back(promise);
                }
            }
        }
    }

    return promise::when(promises);
}

promise::Promise<void> GroupChatRoom::requestRevokeAccessToNode(mega::MegaNode *node)
{
    std::vector<ApiPromise> promises;

    mega::MegaHandleList *megaHandleList = parent.mKarereClient.api.sdk.getAttachmentAccess(mChatid, node->getHandle());

    for (unsigned int j = 0; j < megaHandleList->size(); ++j)
    {
        ApiPromise promise = requestRevokeAccess(node, megaHandleList->get(j));
        promises.push_back(promise);
    }

    delete megaHandleList;

    return promise::when(promises);
}

IApp::IGroupChatListItem* GroupChatRoom::addAppItem()
{
    auto list = parent.mKarereClient.app.chatListHandler();
    return list ? list->addGroupChatItem(*this) : nullptr;
}

//Create chat or receive an invitation
GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const mega::MegaTextChat& aChat)
:ChatRoom(parent, aChat.getHandle(), true, aChat.getShard(),
  (chatd::Priv)aChat.getOwnPrivilege(), aChat.getCreationTime(), aChat.isArchived()),
  mRoomGui(nullptr)
{
    // Initialize list of peers and fetch their names
    auto peers = aChat.getPeerList();
    std::vector<promise::Promise<void>> promises;
    if (peers)
    {
        mNumPeers = peers->size();
        for (int i = 0; i < mNumPeers; i++)
        {
            auto handle = peers->getPeerHandle(i);
            assert(handle != parent.mKarereClient.myHandle());
            mPeers[handle] = new Member(*this, handle, (chatd::Priv)peers->getPeerPrivilege(i)); //may try to access mContactGui, but we have set it to nullptr, so it's ok
            promises.push_back(mPeers[handle]->nameResolved());
        }
    }
    // If there is not any promise at vector promise, promise::when is resolved directly
    mMemberNamesResolved = promise::when(promises);

    // Save Chatroom into DB
    auto db = parent.mKarereClient.db;
    bool isPublicChat = aChat.isPublicChat();
    db.query("insert or replace into chats(chatid, shard, peer, peer_priv, "
             "own_priv, ts_created, archived, mode) values(?,?,-1,0,?,?,?,?)",
             mChatid, mShardNo, mOwnPriv, aChat.getCreationTime(), aChat.isArchived(), isPublicChat);
    db.query("delete from chat_peers where chatid=?", mChatid); // clean any obsolete data
    SqliteStmt stmt(db, "insert into chat_peers(chatid, userid, priv) values(?,?,?)");
    for (auto& m: mPeers)
    {
        stmt << mChatid << m.first << m.second->mPriv;
        stmt.step();
        stmt.reset().clearBind();
    }

    // Initialize unified-key, if any (note private chats may also have unfied-key if user participated while chat was public)
    const char *unifiedKeyPtr = aChat.getUnifiedKey();
    assert(!(isPublicChat && !unifiedKeyPtr));
    std::shared_ptr<std::string> unifiedKey;
    int isUnifiedKeyEncrypted = strongvelope::kDecrypted;
    if (unifiedKeyPtr)
    {
        std::string unifiedKeyB64(unifiedKeyPtr);
        unifiedKey.reset(new std::string);
        int len = ::mega::Base64::atob(unifiedKeyB64, *unifiedKey);
        if (len != strongvelope::SVCRYPTO_KEY_SIZE + ::mega::MegaClient::USERHANDLE)
        {
            KR_LOG_ERROR("Invalid size for unified key");
            isUnifiedKeyEncrypted = strongvelope::kUndecryptable;
            parent.mKarereClient.api.callIgnoreResult(&::mega::MegaApi::sendEvent, 99002, "invalid unified-key detected");
        }
        else
        {
            isUnifiedKeyEncrypted = strongvelope::kEncrypted;
        }

        // Save (still) encrypted unified key
        Buffer unifiedKeyBuf;
        unifiedKeyBuf.write(0, (uint8_t)isUnifiedKeyEncrypted);  // prefix to indicate it's encrypted
        unifiedKeyBuf.append(unifiedKey->data(), unifiedKey->size());
        db.query("update chats set unified_key = ? where chatid = ?", unifiedKeyBuf, mChatid);
    }

    // Initialize chatd::Client (and strongvelope)
    initWithChatd(isPublicChat, unifiedKey, isUnifiedKeyEncrypted);

    // Initialize title, if any
    std::string title = aChat.getTitle() ? aChat.getTitle() : "";
    initChatTitle(title, strongvelope::kEncrypted, true);

    mRoomGui = addAppItem();
    mIsInitializing = false;
}

//Resume from cache
GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const uint64_t& chatid,
    unsigned char aShard, chatd::Priv aOwnPriv, int64_t ts, bool aIsArchived,
    const std::string& title, int isTitleEncrypted, bool publicChat, std::shared_ptr<std::string> unifiedKey, int isUnifiedKeyEncrypted)
    :ChatRoom(parent, chatid, true, aShard, aOwnPriv, ts, aIsArchived),
    mRoomGui(nullptr)
{
    // Initialize list of peers
    SqliteStmt stmt(parent.mKarereClient.db, "select userid, priv from chat_peers where chatid=?");
    stmt << mChatid;
    std::vector<promise::Promise<void> > promises;
    while(stmt.step())
    {
        promises.push_back(addMember(stmt.uint64Col(0), (chatd::Priv)stmt.intCol(1), false));
    }
    mMemberNamesResolved = promise::when(promises);

    // Initialize chatd::Client (and strongvelope)
    initWithChatd(publicChat, unifiedKey, isUnifiedKeyEncrypted);

    // Initialize title, if any
    initChatTitle(title, isTitleEncrypted);

    mRoomGui = addAppItem();
    mIsInitializing = false;
}

//Load chatLink
GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const uint64_t& chatid,
    unsigned char aShard, chatd::Priv aOwnPriv, int64_t ts, bool aIsArchived, const std::string& title,
    const uint64_t publicHandle, std::shared_ptr<std::string> unifiedKey, int aNumPeers, std::string aUrl)
:ChatRoom(parent, chatid, true, aShard, aOwnPriv, ts, aIsArchived, title),
  mRoomGui(nullptr), mNumPeers(aNumPeers)
{
    initWithChatd(true, unifiedKey, 0, publicHandle); // strongvelope only needs the public handle in preview mode (to fetch user attributes via `mcuga`)
    mChat->setPublicHandle(publicHandle);   // chatd always need to know the public handle in preview mode (to send HANDLEJOIN)
    mUrl = aUrl;

    //save to db
    auto db = parent.mKarereClient.db;

    Buffer unifiedKeyBuf;
    unifiedKeyBuf.write(0, (uint8_t)strongvelope::kDecrypted);  // prefix to indicate it's decrypted
    unifiedKeyBuf.append(unifiedKey->data(), unifiedKey->size());

    db.query(
        "insert or replace into chats(chatid, shard, peer, peer_priv, "
        "own_priv, ts_created, mode, unified_key) values(?,?,-1,0,?,?,2,?)",
        mChatid, mShardNo, mOwnPriv, mCreationTs, unifiedKeyBuf);

    initChatTitle(title, strongvelope::kDecrypted, true);

    mRoomGui = addAppItem();
    mIsInitializing = false;
}

void GroupChatRoom::initWithChatd(bool isPublic, std::shared_ptr<std::string> unifiedKey, int isUnifiedKeyEncrypted, Id ph)
{
    karere::SetOfIds users;
    Id myHandle = parent.mKarereClient.myHandle();

    //Don't add my own handle in preview mode because previewers are not chat members
    if(myHandle != Id::null() && !ph.isValid())
    {
        users.insert(myHandle);
    }

    for (auto& peer: mPeers)
    {
        users.insert(peer.first);
    }

    createChatdChat(users, isPublic, unifiedKey, isUnifiedKeyEncrypted, ph);
}

void GroupChatRoom::connect(const char *url)
{
    if (chat().onlineState() != chatd::kChatStateOffline)
        return;

    mChat->connect(url);
}

promise::Promise<void> GroupChatRoom::memberNamesResolved() const
{
    return mMemberNamesResolved;
}

IApp::IPeerChatListItem* PeerChatRoom::addAppItem()
{
    auto list = parent.mKarereClient.app.chatListHandler();
    return list ? list->addPeerChatItem(*this) : nullptr;
}

PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const uint64_t& chatid,
    unsigned char aShard, chatd::Priv aOwnPriv, const uint64_t& peer, chatd::Priv peerPriv, int64_t ts, bool aIsArchived)
:ChatRoom(parent, chatid, false, aShard, aOwnPriv, ts, aIsArchived), mPeer(peer),
  mPeerPriv(peerPriv),
  mRoomGui(nullptr)
{
    initContact(peer);
    initWithChatd();
    mRoomGui = addAppItem();
    mIsInitializing = false;
}

PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const mega::MegaTextChat& chat)
    :ChatRoom(parent, chat.getHandle(), false, chat.getShard(),
     (chatd::Priv)chat.getOwnPrivilege(), chat.getCreationTime(), chat.isArchived()),
      mPeer(getSdkRoomPeer(chat)), mPeerPriv(getSdkRoomPeerPriv(chat)), mRoomGui(nullptr)
{
    parent.mKarereClient.db.query("insert into chats(chatid, shard, peer, peer_priv, own_priv, ts_created, archived) values (?,?,?,?,?,?,?)",
        mChatid, mShardNo, mPeer, mPeerPriv, mOwnPriv, chat.getCreationTime(), chat.isArchived());
//just in case
    parent.mKarereClient.db.query("delete from chat_peers where chatid = ?", mChatid);

    KR_LOG_DEBUG("Added 1on1 chatroom '%s' from API",  ID_CSTR(mChatid));

    initContact(mPeer);
    initWithChatd();
    mRoomGui = addAppItem();
    mIsInitializing = false;
}
PeerChatRoom::~PeerChatRoom()
{
    if (mRoomGui && !parent.mKarereClient.isTerminated())
    {
        parent.mKarereClient.app.chatListHandler()->removePeerChatItem(*mRoomGui);
    }

    if (parent.mKarereClient.mChatdClient)
    {
        parent.mKarereClient.mChatdClient->leave(mChatid);
    }
}

void PeerChatRoom::initContact(const uint64_t& peer)
{
    mContact = parent.mKarereClient.contactList->contactFromUserId(peer);
    mEmail = mContact ? mContact->email() : "Inactive account";
    if (mContact)
    {
        mContact->attachChatRoom(*this);
    }
    else    // 1on1 with ex-user
    {
        mUsernameAttrCbId = parent.mKarereClient.userAttrCache().
                getAttr(peer, USER_ATTR_FULLNAME, this,[](Buffer* data, void* userp)
        {
            //even if both first and last name are null, the data is at least
            //one byte - the firstname-size-prefix, which will be zero
            auto self = static_cast<PeerChatRoom*>(userp);
            if (!data || data->empty() || (*data->buf() == 0))
            {
                self->updateTitle(self->mEmail);
            }
            else
            {
                self->updateTitle(std::string(data->buf()+1, data->dataSize()-1));
            }
        });

        if (mTitleString.empty()) // user attrib fetch was not synchronous
        {
            updateTitle(mEmail);
            assert(!mTitleString.empty());
        }
    }
}

uint64_t PeerChatRoom::getSdkRoomPeer(const ::mega::MegaTextChat& chat)
{
    auto peers = chat.getPeerList();
    assert(peers);
    assert(peers->size() == 1);
    return peers->getPeerHandle(0);
}

chatd::Priv PeerChatRoom::getSdkRoomPeerPriv(const mega::MegaTextChat &chat)
{
    auto peers = chat.getPeerList();
    assert(peers);
    assert(peers->size() == 1);
    return (chatd::Priv) peers->getPeerPrivilege(0);
}

bool ChatRoom::syncOwnPriv(chatd::Priv priv)
{
    if (mOwnPriv == priv)
    {
        return false;
    }

    if(previewMode())
    {
        assert(mOwnPriv == chatd::PRIV_RDONLY
               || mOwnPriv == chatd::PRIV_NOTPRESENT);  // still in preview, but ph is invalid

        if (priv >= chatd::PRIV_RDONLY)
        {
            //Join
            mChat->setPublicHandle(Id::inval());

            //Remove preview mode flag from DB
            parent.mKarereClient.db.query("update chats set mode = '1' where chatid = ?", mChatid);
        }
    }

    mOwnPriv = priv;
    parent.mKarereClient.db.query("update chats set own_priv = ? where chatid = ?", mOwnPriv, mChatid);
    return true;
}

bool ChatRoom::syncArchive(bool aIsArchived)
{
    if (mIsArchived == aIsArchived)
        return false;

    mIsArchived = aIsArchived;
    parent.mKarereClient.db.query("update chats set archived = ? where chatid = ?", mIsArchived, mChatid);

    return true;
}

bool PeerChatRoom::syncPeerPriv(chatd::Priv priv)
{
    if (mPeerPriv == priv)
        return false;

    mPeerPriv = priv;
    parent.mKarereClient.db.query("update chats set peer_priv = ? where chatid = ?", mPeerPriv, mChatid);

    return true;
}

bool PeerChatRoom::syncWithApi(const mega::MegaTextChat &chat)
{
    bool changed = syncOwnPriv((chatd::Priv) chat.getOwnPrivilege());   // returns true if own privilege has changed
    bool changedArchived = syncArchive(chat.isArchived());
    changed |= changedArchived;
    changed |= syncPeerPriv((chatd::Priv)chat.getPeerList()->getPeerPrivilege(0));

    if (changedArchived)
    {
        mIsArchived = chat.isArchived();
        onArchivedChanged(mIsArchived);
    }
    return changed;
}

promise::Promise<void> GroupChatRoom::addMember(uint64_t userid, chatd::Priv priv, bool saveToDb)
{
    assert(userid != parent.mKarereClient.myHandle());

    auto it = mPeers.find(userid);
    if (it != mPeers.end())
    {
        if (it->second->mPriv == priv)
        {
            saveToDb = false;
        }
        else
        {
            it->second->mPriv = priv;
        }
    }
    else
    {
        mPeers.emplace(userid, new Member(*this, userid, priv)); //usernames will be updated when the Member object gets the username attribute

        if (parent.mKarereClient.initState() >= Client::kInitHasOnlineSession)
        {
            if (!parent.mKarereClient.contactList->isExContact(userid))
            {
                parent.mKarereClient.presenced().addPeer(userid);
            }
        }
    }
    if (saveToDb)
    {
        parent.mKarereClient.db.query("insert or replace into chat_peers(chatid, userid, priv) values(?,?,?)",
            mChatid, userid, priv);
    }

    return mPeers[userid]->nameResolved();
}

bool GroupChatRoom::removeMember(uint64_t userid)
{
    KR_LOG_DEBUG("GroupChatRoom[%s]: Removed member %s", ID_CSTR(mChatid), ID_CSTR(userid));

    auto it = mPeers.find(userid);
    if (it == mPeers.end())
    {
        KR_LOG_WARNING("GroupChatRoom::removeMember for a member that we don't have, ignoring");
        return false;
    }

    delete it->second;
    mPeers.erase(it);    
    parent.mKarereClient.presenced().removePeer(userid);
    parent.mKarereClient.db.query("delete from chat_peers where chatid=? and userid=?", mChatid, userid);

    return true;
}

promise::Promise<void> GroupChatRoom::setPrivilege(karere::Id userid, chatd::Priv priv)
{
    auto wptr = getDelTracker();
    return parent.mKarereClient.api.callIgnoreResult(&::mega::MegaApi::updateChatPermissions, chatid(), userid.val, priv)
    .then([this, wptr, userid, priv]()
    {
        wptr.throwIfDeleted();
        if (userid == parent.mKarereClient.myHandle())
        {
            parent.mKarereClient.db.query("update chats set own_priv=? where chatid=?", priv, mChatid);
        }
        else
        {
            parent.mKarereClient.db.query("update chat_peers set priv=? where chatid=? and userid=?", priv, mChatid, userid);
        }
    });
}

promise::Promise<void> ChatRoom::truncateHistory(karere::Id msgId)
{
    auto wptr = getDelTracker();
    return parent.mKarereClient.api.callIgnoreResult(
                &::mega::MegaApi::truncateChat,
                chatid(),
                msgId)
    .then([this, wptr]()
    {
        wptr.throwIfDeleted();
        // TODO: update indexes, last message and so on
    });
}

bool ChatRoom::isCallInProgress() const
{
    return parent.mKarereClient.isCallInProgress(mChatid);
}

promise::Promise<void> ChatRoom::archiveChat(bool archive)
{
    auto wptr = getDelTracker();
    return parent.mKarereClient.api.callIgnoreResult(&::mega::MegaApi::archiveChat, chatid(), archive)
    .then([this, wptr, archive]()
    {
        wptr.throwIfDeleted();

        bool archiveChanged = syncArchive(archive);
        if (archiveChanged)
        {
            onArchivedChanged(archive);
        }
    });
}

void GroupChatRoom::deleteSelf()
{
    //have to post a delete on the event loop, as there may be pending
    //events related to the chatroom/strongvelope instance
    auto wptr = weakHandle();
    marshallCall([wptr, this]()
    {
        if (wptr.deleted())
        {
            return;
        }
        delete this;
    }, parent.mKarereClient.appCtx);
}

ChatRoomList::ChatRoomList(Client& aClient)
:mKarereClient(aClient)
{}

void ChatRoomList::loadFromDb()
{
    auto db = mKarereClient.db;

    //We need to ensure that the DB does not contain any record related with a preview
    SqliteStmt stmtPreviews(db, "select chatid from chats where mode = '2'");
    while(stmtPreviews.step())
    {
        Id chatid = stmtPreviews.uint64Col(0);
        previewCleanup(chatid);
    }

    SqliteStmt stmt(db, "select chatid, ts_created ,shard, own_priv, peer, peer_priv, title, archived, mode, unified_key from chats");
    while(stmt.step())
    {
        auto chatid = stmt.uint64Col(0);
        if (find(chatid) != end())
        {
            KR_LOG_WARNING("ChatRoomList: Attempted to load from db cache a chatid that is already in memory");
            continue;
        }
        auto peer = stmt.uint64Col(4);
        ChatRoom* room;
        if (peer != uint64_t(-1))
        {
            room = new PeerChatRoom(*this, chatid, stmt.intCol(2), (chatd::Priv)stmt.intCol(3), peer, (chatd::Priv)stmt.intCol(5), stmt.intCol(1), stmt.intCol(7));
        }
        else
        {
            std::shared_ptr<std::string> unifiedKey;
            int isUnifiedKeyEncrypted = strongvelope::kDecrypted;

            Buffer unifiedKeyBuf;
            stmt.blobCol(9, unifiedKeyBuf);
            if (!unifiedKeyBuf.empty())
            {
                const char *pos = unifiedKeyBuf.buf();
                isUnifiedKeyEncrypted = (uint8_t)*pos;  pos++;
                size_t len = unifiedKeyBuf.size() - 1;
                assert( (isUnifiedKeyEncrypted == strongvelope::kDecrypted && len == 16)
                        || (isUnifiedKeyEncrypted == strongvelope::kEncrypted && len == 24)  // encrypted version includes invitor's userhandle (8 bytes)
                        || (isUnifiedKeyEncrypted));
                unifiedKey.reset(new std::string(pos, len));
            }

            // Get title and check if it's encrypted or not
            std::string auxTitle;
            int isTitleEncrypted = strongvelope::kDecrypted;

            Buffer titleBuf;
            stmt.blobCol(6, titleBuf);
            if (!titleBuf.empty())
            {
                const char *posTitle = titleBuf.buf();
                isTitleEncrypted = (uint8_t)*posTitle;  posTitle++;
                size_t len = titleBuf.size() - 1;
                auxTitle.assign(posTitle, len);
            }

            room = new GroupChatRoom(*this, chatid, stmt.intCol(2), (chatd::Priv)stmt.intCol(3), stmt.intCol(1), stmt.intCol(7), auxTitle, isTitleEncrypted, stmt.intCol(8), unifiedKey, isUnifiedKeyEncrypted);
        }
        emplace(chatid, room);
    }
}
void ChatRoomList::addMissingRoomsFromApi(const mega::MegaTextChatList& rooms, SetOfIds& chatids)
{
    auto size = rooms.size();
    for (int i = 0; i < size; i++)
    {
        auto& apiRoom = *rooms.get(i);
        auto chatid = apiRoom.getHandle();
        auto it = find(chatid);
        if (it != end())
            continue;   // chatroom already known

        ChatRoom* room = addRoom(apiRoom);
        chatids.insert(chatid);

        if (mKarereClient.connected())
        {
            KR_LOG_DEBUG("...connecting new room to chatd...");
            room->connect();
        }
        else
        {
            KR_LOG_DEBUG("...client is not connected, not connecting new room");
        }
    }
}

ChatRoom* ChatRoomList::addRoom(const mega::MegaTextChat& apiRoom)
{
    auto chatid = apiRoom.getHandle();

    ChatRoom* room;
    if(apiRoom.isGroup())
    {
        //We need to ensure that unified key exists before decrypt title for public chats. So we decrypt title inside ctor
        room = new GroupChatRoom(*this, apiRoom); //also writes it to cache
    }
    else    // 1on1
    {
        room = new PeerChatRoom(*this, apiRoom);
    }

#ifndef NDEBUG
    auto ret =
#endif
    emplace(chatid, room);
    assert(ret.second); //we should not have that room
    return room;
}

void ChatRoom::notifyExcludedFromChat()
{
    if (mAppChatHandler)
        mAppChatHandler->onExcludedFromChat();
    auto listItem = roomGui();
    if (listItem)
        listItem->onExcludedFromChat();
}

void ChatRoom::notifyRejoinedChat()
{
    if (mAppChatHandler)
        mAppChatHandler->onRejoinedChat();
    auto listItem = roomGui();
    if (listItem)
        listItem->onRejoinedChat();
}

void ChatRoomList::removeRoomPreview(Id chatid)
{
    auto wptr = mKarereClient.weakHandle();
    marshallCall([wptr, this, chatid]()
    {
        if (wptr.deleted())
        {
            return;
        }

        auto it = find(chatid);
        if (it == end())
        {
            CHATD_LOG_WARNING("removeRoomPreview: room not in chat list");
            return;
        }
        if (!it->second->previewMode())
        {
            CHATD_LOG_WARNING("removeRoomPreview: room is not a preview");
            return;
        }

        GroupChatRoom *groupchat = (GroupChatRoom*)it->second;
        erase(it);
        delete groupchat;
    },mKarereClient.appCtx);
}

void GroupChatRoom::setRemoved()
{
    mOwnPriv = chatd::PRIV_NOTPRESENT;
    parent.mKarereClient.db.query("update chats set own_priv=? where chatid=?", mOwnPriv, mChatid);
    notifyExcludedFromChat();
}

void Client::onChatsUpdate(mega::MegaApi*, mega::MegaTextChatList* rooms)
{
    if (!rooms)
        return;
    std::shared_ptr<mega::MegaTextChatList> copy(rooms->copy());
    char* pscsn = api.sdk.getSequenceNumber();
    std::string scsn;
    if (pscsn)
    {
        scsn = pscsn;
        delete[] pscsn;
    }
#ifndef NDEBUG
    dumpChatrooms(*copy);
#endif
    assert(mContactsLoaded);
    auto wptr = weakHandle();
    marshallCall([wptr, this, copy, scsn]()
    {
        if (wptr.deleted())
        {
            return;
        }

        chats->onChatsUpdate(*copy);
    }, appCtx);
}

void ChatRoomList::onChatsUpdate(mega::MegaTextChatList& rooms)
{
    SetOfIds added;
    addMissingRoomsFromApi(rooms, added);
    auto count = rooms.size();
    for (int i = 0; i < count; i++)
    {
        auto apiRoom = rooms.get(i);
        auto chatid = apiRoom->getHandle();
        if (added.has(chatid)) //room was just added, no need to sync
            continue;

        auto it = find(chatid);
        auto localRoom = (it != end()) ? it->second : nullptr;
        assert(localRoom);
        if (localRoom)
        {
            localRoom->syncWithApi(*apiRoom);
        }
        else
        {
            KR_LOG_ERROR("Chatroom %s notified by API is not found", ID_CSTR(chatid));
        }
    }
}

ChatRoomList::~ChatRoomList()
{
    for (auto& room: *this)
        delete room.second;
}

promise::Promise<void> GroupChatRoom::decryptTitle()
{
    assert(!mEncryptedTitle.empty());

    Buffer buf(mEncryptedTitle.size());    
    try
    {
        size_t decLen = base64urldecode(mEncryptedTitle.c_str(), mEncryptedTitle.size(), buf.buf(), buf.bufSize());
        buf.setDataSize(decLen);
    }
    catch(std::exception& e)
    {
        KR_LOG_ERROR("Failed to base64-decode chat title for chat %s: %s. Falling back to member names", ID_CSTR(mChatid), e.what());

        updateTitleInDb(mEncryptedTitle, strongvelope::kUndecryptable);
        makeTitleFromMemberNames();

        return promise::Error(e.what());
    }

    auto wptr = getDelTracker();
    promise::Promise<std::string> pms = chat().crypto()->decryptChatTitleFromApi(buf);
    return pms.then([wptr, this](const std::string title)
    {
        wptr.throwIfDeleted();

        // Update title (also in cache) and notify that has changed
        handleTitleChange(title, true);
    })
    .fail([wptr, this](const promise::Error& err)
    {
        wptr.throwIfDeleted();

        KR_LOG_ERROR("Error decrypting chat title for chat %s: %s. Falling back to member names.", ID_CSTR(chatid()), err.what());

        updateTitleInDb(mEncryptedTitle, strongvelope::kUndecryptable);
        makeTitleFromMemberNames();

        return err;
    });
}

void GroupChatRoom::updateTitleInDb(const std::string &title, int isEncrypted)
{
    KR_LOG_DEBUG("Title update in cache");
    Buffer titleBuf;
    titleBuf.write(0, (uint8_t)isEncrypted);
    titleBuf.append(title.data(), title.size());
    parent.mKarereClient.db.query("update chats set title=? where chatid=?", titleBuf, mChatid);
}

void GroupChatRoom::makeTitleFromMemberNames()
{
    mHasTitle = false;
    std::string newTitle;
    if (mPeers.empty())
    {
        time_t ts = mCreationTs;
        const struct tm *time = localtime(&ts);
        char date[18];
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", time);
        newTitle = "Chat created on ";
        newTitle.append(date);
    }
    else
    {
        for (auto& m: mPeers)
        {
            //name has binary layout
            auto& name = m.second->mName;
            assert(!name.empty()); //is initialized to '\0', so is never empty
            if (name.size() <= 1)
            {
                auto& email = m.second->mEmail;
                if (!email.empty())
                    newTitle.append(email).append(", ");
                else
                    newTitle.append("..., ");
            }
            else
            {
                newTitle.append(name.substr(1)).append(", ");
            }
        }
        newTitle.resize(newTitle.size()-2); //truncate last ", "
    }
    assert(!newTitle.empty());
    if (newTitle == mTitleString)
    {
        KR_LOG_DEBUG("makeTitleFromMemberNames: same title than existing one, skipping update");
        return;
    }

    mTitleString = newTitle;
    notifyTitleChanged();
}

promise::Promise<void> GroupChatRoom::setTitle(const std::string& title)
{
    auto wptr = getDelTracker();
    return chat().crypto()->encryptChatTitle(title)
    .then([wptr, this](const std::shared_ptr<Buffer>& buf)
    {
        wptr.throwIfDeleted();
        auto b64 = base64urlencode(buf->buf(), buf->dataSize());
        return parent.mKarereClient.api.callIgnoreResult(&::mega::MegaApi::setChatTitle, chatid(),
            b64.c_str());
    })
    .then([wptr, this, title]()
    {
        wptr.throwIfDeleted();
        if (title.empty())
        {
            clearTitle();
        }
    });
}

GroupChatRoom::~GroupChatRoom()
{
    removeAppChatHandler();

    if (mRoomGui && !parent.mKarereClient.isTerminated())
    {
        parent.mKarereClient.app.chatListHandler()->removeGroupChatItem(*mRoomGui);
    }

    if (previewMode())
    {
        parent.previewCleanup(mChatid);
    }

    if (parent.mKarereClient.mChatdClient)
    {
        parent.mKarereClient.mChatdClient->leave(mChatid);
    }

    for (auto& m: mPeers)
    {
        delete m.second;
    }
}

promise::Promise<void> GroupChatRoom::leave()
{
    auto wptr = getDelTracker();

    return parent.mKarereClient.api.callIgnoreResult(&mega::MegaApi::removeFromChat, mChatid, mega::INVALID_HANDLE)
    .fail([](const promise::Error& err) -> Promise<void>
    {
        if (err.code() == ::mega::MegaError::API_EARGS) //room does not actually exist on API, ignore room and remove it locally
            return promise::_Void();
        else
            return err;
    })
    .then([this, wptr]()
    {
        wptr.throwIfDeleted();
        setRemoved();
    });
}

promise::Promise<void> GroupChatRoom::invite(uint64_t userid, chatd::Priv priv)
{
    auto wptr = getDelTracker();

    //Add new user to strongvelope set of users
    promise::Promise<std::string> pms;
    if (mHasTitle && !publicChat())
    {
        pms = chat().crypto()->encryptChatTitle(mTitleString, userid)
        .then([](const std::shared_ptr<Buffer>& buf)
        {
            return base64urlencode(buf->buf(), buf->dataSize());;
        });
    }
    else
    {
        pms = promise::Promise<std::string>(std::string());
    }

    return pms
    .then([this, wptr, userid, priv](const std::string& title)
    {
        wptr.throwIfDeleted();
        ApiPromise invitePms;
        if (publicChat())
        {
            invitePms = chat().crypto()->encryptUnifiedKeyForAllParticipants(userid)
            .then([wptr, this, userid, priv](chatd::KeyCommand* encKey)
             {
                //Get peer unified key
                auto useruk = encKey->getKeyByUserId(userid);

                //Get creator handle in binary
                uint64_t invitorHandle = chat().client().mKarereClient->myHandle().val;

                //Append [invitorhandle+uk]
                std::string uKeyBin((const char*)&invitorHandle, sizeof(invitorHandle));
                uKeyBin.append(useruk->buf(), useruk->size());

                //Encode [invitorhandle+uk] to B64
                std::string uKeyB64;
                mega::Base64::btoa(uKeyBin, uKeyB64);

                return parent.mKarereClient.api.call(&mega::MegaApi::inviteToPublicChat, mChatid, userid, priv,
                    uKeyB64.c_str());
             });
        }
        else
        {
            invitePms = parent.mKarereClient.api.call(&mega::MegaApi::inviteToChat, mChatid, userid, priv,
                title.empty() ? nullptr : title.c_str());
        }
        return invitePms
        .then([this, wptr, userid, priv](ReqResult)
        {
            wptr.throwIfDeleted();
            addMember(userid, priv, true)
            .then([wptr, this]()
            {
                wptr.throwIfDeleted();
                if (!mHasTitle)
                {
                    makeTitleFromMemberNames();
                }
            });
        });
    });
}

promise::Promise<void> GroupChatRoom::autojoinPublicChat(uint64_t ph)
{
    Id myHandle(parent.mKarereClient.myHandle());

    return chat().crypto()->encryptUnifiedKeyToUser(myHandle)
    .then([this, myHandle, ph](std::string key) -> ApiPromise
    {
        //Append [invitorhandle+uk]
        std::string uKeyBin((const char*)&myHandle, sizeof(myHandle.val));
        uKeyBin.append(key.data(), key.size());

        //Encode [invitorhandle+uk] to B64
        std::string uKeyB64;
        mega::Base64::btoa(uKeyBin, uKeyB64);

        return parent.mKarereClient.api.call(&mega::MegaApi::chatLinkJoin, ph, uKeyB64.c_str());
    })
    .then([this, myHandle](ReqResult)
    {
        onUserJoin(parent.mKarereClient.myHandle(), chatd::PRIV_FULL);
    });
 }

//chatd::Listener::init
void ChatRoom::init(chatd::Chat& chat, chatd::DbInterface*& dbIntf)
{
    mChat = &chat;
    dbIntf = new ChatdSqliteDb(*mChat, parent.mKarereClient.db);
    if (mAppChatHandler)
    {
        setAppChatHandler(mAppChatHandler);
    }
}

void ChatRoom::setAppChatHandler(IApp::IChatHandler* handler)
{
    if (mAppChatHandler)
        throw std::runtime_error("App chat handler is already set, remove it first");

    mAppChatHandler = handler;
    chatd::DbInterface* dummyIntf = nullptr;
// mAppChatHandler->init() may rely on some events, so we need to set mChatWindow as listener before
// calling init(). This is safe, as and we will not get any async events before we
//return to the event loop
    mChat->setListener(mAppChatHandler);
    mAppChatHandler->init(*mChat, dummyIntf);
}

void ChatRoom::removeAppChatHandler()
{
    if (!mAppChatHandler)
        return;
    mAppChatHandler = nullptr;
    mChat->setListener(this);
}

bool ChatRoom::hasChatHandler() const
{
    return mAppChatHandler != NULL;
}

void GroupChatRoom::onUserJoin(Id userid, chatd::Priv privilege)
{
    auto it = mPeers.find(userid);
    if (it != mPeers.end() && it->second->mPriv == privilege)
    {
        return;
    }

    if (userid == parent.mKarereClient.myHandle())
    {
        syncOwnPriv(privilege);
    }
    else
    {
        auto wptr = weakHandle();
        addMember(userid, privilege, true)
        .then([wptr, this]()
        {
            wptr.throwIfDeleted();
            if (!mHasTitle)
            {
                makeTitleFromMemberNames();
            }
        });
    }
    if (mRoomGui)
    {
        mRoomGui->onUserJoin(userid, privilege);
    }
}

void GroupChatRoom::onUserLeave(Id userid)
{
    if (userid == parent.mKarereClient.myHandle())
    {
        setRemoved();
    }
    else if (userid == Id::null())
    {
        // preview is not allowed anymore, notify the user and clean cache
        assert(previewMode());

        setRemoved();
    }
    else
    {
        if (removeMember(userid) && !mHasTitle)
        {
            makeTitleFromMemberNames();
        }

        if (mRoomGui)
            mRoomGui->onUserLeave(userid);
    }
}

void PeerChatRoom::onUserJoin(Id userid, chatd::Priv privilege)
{
    if (userid == parent.mKarereClient.myHandle())
        syncOwnPriv(privilege);
    else if (userid.val == mPeer)
        syncPeerPriv(privilege);
    else
        KR_LOG_ERROR("PeerChatRoom: Bug: Received JOIN event from chatd for a third user, ignoring");
}
void PeerChatRoom::onUserLeave(Id userid)
{
    KR_LOG_ERROR("PeerChatRoom: Bug: Received leave event for user %s from chatd on a permanent chat, ignoring", ID_CSTR(userid));
}

void ChatRoom::onLastTextMessageUpdated(const chatd::LastTextMsg& msg)
{
    if (mIsInitializing)
    {
        auto wptr = weakHandle();
        marshallCall([=]()
        {
            if (wptr.deleted())
                return;
            auto display = roomGui();
            if (display)
                display->onLastMessageUpdated(msg);
        }, parent.mKarereClient.appCtx);
    }
    else
    {
        auto display = roomGui();
        if (display)
           display->onLastMessageUpdated(msg);
    }
}

//chatd notification
void ChatRoom::onOnlineStateChange(chatd::ChatState state)
{
    auto display = roomGui();
    if (display)
    {
        display->onChatOnlineState(state);
    }
}

void ChatRoom::onMsgOrderVerificationFail(const chatd::Message &msg, chatd::Idx idx, const std::string &errmsg)
{
    KR_LOG_ERROR("msgOrderFail[chatid: %s, msgid %s, idx %d, userid %s]: %s",
        ID_CSTR(mChatid), ID_CSTR(msg.id()), idx, ID_CSTR(msg.userid), errmsg.c_str());
}

void ChatRoom::onRecvNewMessage(chatd::Idx idx, chatd::Message& msg, chatd::Message::Status status)
{
    if ( (msg.type == chatd::Message::kMsgTruncate)   // truncate received from a peer or from myself in another client
         || (msg.userid != parent.mKarereClient.myHandle() && status == chatd::Message::kNotSeen) )  // new (unseen) message received from a peer
    {
        parent.mKarereClient.app.onChatNotification(mChatid, msg, status, idx);
    }

    if (msg.type == chatd::Message::kMsgChatTitle)
    {
        std::string title(msg.buf(), msg.size());

        // Update title and notify that has changed
        ((GroupChatRoom *) this)->handleTitleChange(title);
    }
}

void GroupChatRoom::handleTitleChange(const std::string &title, bool saveToDB)
{
    if (saveToDB)
    {
        updateTitleInDb(title, strongvelope::kDecrypted);
    }

    if (mTitleString == title)
    {
        KR_LOG_DEBUG("Same title has already been notified, skipping update");
        return;
    }

    mTitleString = title;
    mHasTitle = true;

    notifyTitleChanged();
}

void ChatRoom::onMessageEdited(const chatd::Message& msg, chatd::Idx idx)
{
    chatd::Message::Status status = mChat->getMsgStatus(msg, idx);

    //TODO: check a truncate always comes as an edit, even if no history exist at all (new chat)
    // and, if so, remove the block from `onRecvNewMessage()`
    if ( (msg.type == chatd::Message::kMsgTruncate) // truncate received from a peer or from myself in another client
         || (msg.userid != parent.mKarereClient.myHandle() && status == chatd::Message::kNotSeen) )    // received message from a peer, still unseen, was edited / deleted
    {
        parent.mKarereClient.app.onChatNotification(mChatid, msg, status, idx);
    }
}

void ChatRoom::onMessageStatusChange(chatd::Idx idx, chatd::Message::Status status, const chatd::Message& msg)
{
    if (msg.userid != parent.mKarereClient.myHandle() && status == chatd::Message::kSeen)  // received message from a peer changed to seen
    {
        parent.mKarereClient.app.onChatNotification(mChatid, msg, status, idx);
    }
}

void ChatRoom::onUnreadChanged()
{
    auto count = mChat->unreadMsgCount();
    IApp::IChatListItem *room = roomGui();
    if (room)
    {
        room->onUnreadCountChanged(count);
    }
}

void ChatRoom::onPreviewersUpdate()
{
    IApp::IChatListItem *room = roomGui();
    if (room)
    {
        room->onPreviewersCountUpdate(mChat->getNumPreviewers());
    }
}

void ChatRoom::onArchivedChanged(bool archived)
{
    IApp::IChatListItem *room = roomGui();
    if (room)
    {
        room->onChatArchived(archived);
    }
    if (mAppChatHandler)
    {
        mAppChatHandler->onChatArchived(archived);
    }

    // since the archived rooms don't count for the chats with unread messages,
    // we need to notifiy the apps about the changes on unread messages.
    onUnreadChanged();
}

void PeerChatRoom::updateTitle(const std::string& title)
{
    mTitleString = title;
    notifyTitleChanged();
}

void ChatRoom::notifyTitleChanged()
{
    callAfterInit(this, [this]
    {
        auto display = roomGui();
        if (display)
            display->onTitleChanged(mTitleString);

        if (mAppChatHandler)
            mAppChatHandler->onTitleChanged(mTitleString);
    }, parent.mKarereClient.appCtx);
}

void ChatRoom::notifyChatModeChanged()
{
    callAfterInit(this, [this]
    {
        auto display = roomGui();
        if (display)
            display->onChatModeChanged(this->publicChat());

        if (mAppChatHandler)
            mAppChatHandler->onChatModeChanged(this->publicChat());
    }, parent.mKarereClient.appCtx);
}

void GroupChatRoom::enablePreview(uint64_t ph)
{
    // Current priv is PRIV_NOTPRESENT and need to be updated
    mOwnPriv = chatd::PRIV_RDONLY;
    parent.mKarereClient.db.query("update chats set own_priv = ? where chatid = ?", mOwnPriv, mChatid);
    if (mRoomGui)
    {
        mRoomGui->onUserJoin(parent.mKarereClient.myHandle(), mOwnPriv);
    }

    mChat->setPublicHandle(ph);
    chat().disable(false);
    connect();
}

bool GroupChatRoom::publicChat() const
{
    return (mChat->crypto()->isPublicChat());
}

uint64_t GroupChatRoom::getPublicHandle() const
{
    return (mChat->getPublicHandle());
}

unsigned int GroupChatRoom::getNumPreviewers() const
{
    return mChat->getNumPreviewers();
}

// return true if new peer, peer removed or peer's privilege updated
bool GroupChatRoom::previewMode() const
{
    return mChat->previewMode();
}

void ChatRoomList::previewCleanup(Id chatid)
{
    auto db = mKarereClient.db;
    if (db.isOpen())   // upon karere::Client destruction, DB is already closed
    {
        db.query("delete from chat_peers where chatid = ?", chatid);
        db.query("delete from chat_vars where chatid = ?", chatid);
        db.query("delete from chats where chatid = ?", chatid);
        db.query("delete from history where chatid = ?", chatid);
        db.query("delete from manual_sending where chatid = ?", chatid);
        db.query("delete from sending where chatid = ?", chatid);
        db.query("delete from sendkeys where chatid = ?", chatid);
        db.query("delete from node_history where chatid = ?", chatid);
    }
}

promise::Promise<std::shared_ptr<std::string>> GroupChatRoom::unifiedKey()
{
    return mChat->crypto()->getUnifiedKey();
}

int GroupChatRoom::getNumPeers() const
{
    return mNumPeers;
}

// return true if new peer or peer removed. Updates peer privileges as well
bool GroupChatRoom::syncMembers(const mega::MegaTextChat& chat)
{
    UserPrivMap users;
    auto members = chat.getPeerList();
    if (members)
    {
        auto size = members->size();
        for (int i = 0; i < size; i++)
        {
            users.emplace(members->getPeerHandle(i), (chatd::Priv)members->getPeerPrivilege(i));
        }
    }

    auto db = parent.mKarereClient.db;
    bool peersChanged = false;
    for (auto ourIt = mPeers.begin(); ourIt != mPeers.end();)
    {
        auto userid = ourIt->first;
        auto member = ourIt->second;

        auto it = users.find(userid);
        if (it == users.end()) //we have a user that is not in the chatroom anymore
        {
            peersChanged = true;
            ourIt++;    // prevent iterator becoming invalid due to removal
            removeMember(userid);
        }
        else    // existing peer changed privilege
        {
            if (member->mPriv != it->second)
            {
                KR_LOG_DEBUG("GroupChatRoom[%s]:syncMembers: Changed privilege of member %s: %d -> %d",
                     ID_CSTR(chatid()), ID_CSTR(userid), member->mPriv, it->second);

                member->mPriv = it->second;
                db.query("update chat_peers set priv=? where chatid=? and userid=?", member->mPriv, mChatid, userid);
            }
            ourIt++;
        }
    }

    std::vector<promise::Promise<void> > promises;
    for (auto& user: users)
    {
        if (mPeers.find(user.first) == mPeers.end())
        {
            peersChanged = true;
            promises.push_back(addMember(user.first, user.second, true));
        }
    }

    if (peersChanged)
    {
        auto wptr = weakHandle();
        promise::when(promises)
        .then([wptr, this]()
        {
            wptr.throwIfDeleted();
            if (!mHasTitle)
            {
                makeTitleFromMemberNames();
            }
        });
    }

    return peersChanged;
}

void GroupChatRoom::initChatTitle(const std::string &title, int isTitleEncrypted, bool saveToDb)
{
    mHasTitle = (!title.empty() && title.at(0));
    if (mHasTitle)
    {
        if (saveToDb)
        {
            updateTitleInDb(title, isTitleEncrypted);
        }

        switch (isTitleEncrypted)
        {
            case strongvelope::kDecrypted:
                mTitleString = title;
                notifyTitleChanged();
                return;

            case strongvelope::kEncrypted:
                mEncryptedTitle = title;
                decryptTitle()
                .fail([this](const promise::Error& e)
                {
                    KR_LOG_ERROR("GroupChatRoom: failed to decrypt title for chat %s: %s", ID_CSTR(mChatid), e.what());
                });
                return;

            case strongvelope::kUndecryptable:
                KR_LOG_ERROR("Undecryptable chat title for chat %s", ID_CSTR(mChatid));
                // fallback to makeTitleFromMemberNames()
                break;
        }
    }

    // if has no title or it's undecryptable...
    auto wptr = weakHandle();
    mMemberNamesResolved.then([wptr, this]()
    {
        if (wptr.deleted())
            return;

        makeTitleFromMemberNames();
    });
}

void GroupChatRoom::clearTitle()
{
    makeTitleFromMemberNames();
    parent.mKarereClient.db.query("update chats set title=NULL where chatid=?", mChatid);
}

bool GroupChatRoom::syncWithApi(const mega::MegaTextChat& chat)
{
    // Mode changed
    if (!chat.isPublicChat() && publicChat())
    {
        KR_LOG_DEBUG("Chatroom[%s]: API event: mode changed to private", ID_CSTR(mChatid));
        setChatPrivateMode();
        // in case of previewMode, it's also updated in cache
    }

    // Own privilege changed
    auto oldPriv = mOwnPriv;
    bool ownPrivChanged = syncOwnPriv((chatd::Priv) chat.getOwnPrivilege());
    if (ownPrivChanged)
    {
        if (oldPriv == chatd::PRIV_NOTPRESENT)
        {
            if (mOwnPriv != chatd::PRIV_NOTPRESENT)
            {
                // in case chat-link was invalidated during preview, the room was disabled
                // now, we upgrade from (invalid) previewer to participant --> enable it back
                if (mChat->isDisabled())
                {
                    KR_LOG_WARNING("Enable chatroom previously in preview mode");
                    mChat->disable(false);
                }

                // if already connected, need to send a new JOIN to chatd
                if (parent.mKarereClient.connected())
                {
                    KR_LOG_DEBUG("Connecting existing room to chatd after re-join...");
                    if (mChat->onlineState() != ::chatd::ChatState::kChatStateJoining)
                    {
                        mChat->connect();
                    }
                    else
                    {
                        KR_LOG_DEBUG("Skip re-join chatd, since it's already joining right now");
                        parent.mKarereClient.api.callIgnoreResult(&::mega::MegaApi::sendEvent, 99003, "Skip re-join chatd");
                    }
                }
                KR_LOG_DEBUG("Chatroom[%s]: API event: We were re/invited",  ID_CSTR(mChatid));
                notifyRejoinedChat();
            }
        }
        else if (mOwnPriv == chatd::PRIV_NOTPRESENT)
        {
            //we were excluded
            KR_LOG_DEBUG("Chatroom[%s]: API event: We were removed", ID_CSTR(mChatid));
            setRemoved(); // may delete 'this'
            return true;
        }
        else
        {
            KR_LOG_DEBUG("Chatroom[%s]: API event: Our own privilege changed",  ID_CSTR(mChatid));
            onUserJoin(parent.mKarereClient.myHandle(), mOwnPriv);
        }
    }

    // Peer list changes
    bool membersChanged = syncMembers(chat);

    // Title changes
    const char *title = chat.getTitle();
    mHasTitle = (title && title[0]);
    if (mHasTitle)
    {
        if (mEncryptedTitle != title)   // title has changed
        {
            // if the title was already decrypted in cache at startup, the `mEncryptedTitle` won't be initialized yet
            // (the encrypted flavour of the title is saved in cache but overwriten when decrypted)
            // In consequence, the first actionpacket will initialize it and decrypt it once per execution
            mEncryptedTitle = title;
            updateTitleInDb(mEncryptedTitle, strongvelope::kEncrypted);

            decryptTitle()
            .fail([](const promise::Error& err)
            {
                KR_LOG_DEBUG("Can't decrypt chatroom title. In function: GroupChatRoom::syncWithApi. Error: %s", err.what());
            });
        }
    }
    else if (membersChanged)
    {
        KR_LOG_DEBUG("Empty title received for groupchat %s. Peers changed, updating title...", ID_CSTR(mChatid));
        clearTitle();
    }

    bool archiveChanged = syncArchive(chat.isArchived());
    if (archiveChanged)
    {
        onArchivedChanged(mIsArchived);
    }

    KR_LOG_DEBUG("Synced group chatroom %s with API.", ID_CSTR(mChatid));
    return true;
}

void GroupChatRoom::setChatPrivateMode()
{
    //Update strongvelope
    chat().crypto()->setPrivateChatMode();

    //Update cache
    parent.mKarereClient.db.query("update chats set mode = '0' where chatid = ?", mChatid);

    notifyChatModeChanged();
}

GroupChatRoom::Member::Member(GroupChatRoom& aRoom, const uint64_t& user, chatd::Priv aPriv)
: mRoom(aRoom), mHandle(user), mPriv(aPriv), mName("\0", 1)
{
    mNameAttrCbHandle = mRoom.parent.mKarereClient.userAttrCache().getAttr(
        user, USER_ATTR_FULLNAME, this,
        [](Buffer* buf, void* userp)
    {
        auto self = static_cast<Member*>(userp);
        if (buf && !buf->empty())
        {
            self->mName.assign(buf->buf(), buf->dataSize());
        }
        else
        {
            self->mName.assign("\0", 1);
        }
        if (self->mRoom.mAppChatHandler)
        {
            self->mRoom.mAppChatHandler->onMemberNameChanged(self->mHandle, self->mName);
        }

        if (!self->mNameResolved.done())
        {
            self->mNameResolved.resolve();
        }
        else if (self->mRoom.memberNamesResolved().done() && !self->mRoom.mHasTitle)
        {
            self->mRoom.makeTitleFromMemberNames();
        }
    });

    mEmailAttrCbHandle = mRoom.parent.mKarereClient.userAttrCache().getAttr(
        user, USER_ATTR_EMAIL, this,
        [](Buffer* buf, void* userp)
    {
        auto self = static_cast<Member*>(userp);
        if (buf && !buf->empty())
        {
            self->mEmail.assign(buf->buf(), buf->dataSize());
            if (self->mName.size() <= 1 && self->mRoom.memberNamesResolved().done() && !self->mRoom.mHasTitle)
            {
                self->mRoom.makeTitleFromMemberNames();
            }
        }
    });
}

GroupChatRoom::Member::~Member()
{
    mRoom.parent.mKarereClient.userAttrCache().removeCb(mNameAttrCbHandle);
    mRoom.parent.mKarereClient.userAttrCache().removeCb(mEmailAttrCbHandle);
}

promise::Promise<void> GroupChatRoom::Member::nameResolved() const
{
    return mNameResolved;
}

void Client::connectToChatd(bool isInBackground)
{
    mChatdClient->setKeepaliveType(isInBackground);

    for (auto& item: *chats)
    {
        auto& chat = *item.second;
        if (!chat.chat().isDisabled())
        {
            chat.connect();
        }
    }
}

ContactList::ContactList(Client& aClient)
:client(aClient)
{}

void ContactList::loadFromDb()
{
    SqliteStmt stmt(client.db, "select userid, email, visibility, since from contacts");
    while(stmt.step())
    {
        auto userid = stmt.uint64Col(0);
        Contact *contact = new Contact(*this, userid, stmt.stringCol(1), stmt.intCol(2), stmt.int64Col(3), nullptr);
        this->emplace(userid, contact);
    }
}

bool ContactList::addUserFromApi(mega::MegaUser& user)
{
    auto userid = user.getHandle();
    auto it = this->find(userid);
    if (it != this->end())
    {
        Contact *contact = it->second;
        assert(contact);
        int newVisibility = user.getVisibility();
        if (contact->visibility() == newVisibility)
        {
            return false;
        }

        client.db.query("update contacts set visibility = ? where userid = ?",
            newVisibility, userid);
        contact->onVisibilityChanged(newVisibility);
        return true;
    }

    // contact was not created yet
    auto cmail = user.getEmail();
    std::string email(cmail ? cmail : "");
    int visibility = user.getVisibility();
    auto ts = user.getTimestamp();
    client.db.query("insert or replace into contacts(userid, email, visibility, since) values(?,?,?,?)",
            userid, email, visibility, ts);
    Contact *contact = new Contact(*this, userid, email, visibility, ts, nullptr);
    this->emplace(userid, contact);
    KR_LOG_DEBUG("Added new user from API: %s", email.c_str());
    return true;
}

void Contact::onVisibilityChanged(int newVisibility)
{
    assert(newVisibility != mVisibility);
    auto old = mVisibility;
    mVisibility = newVisibility;
    if (mDisplay)
    {
        mDisplay->onVisibilityChanged(newVisibility);
    }

    auto& client = mClist.client;
    if (newVisibility == ::mega::MegaUser::VISIBILITY_HIDDEN || newVisibility == ::mega::MegaUser::VISIBILITY_INACTIVE)
    {
        client.presenced().removePeer(mUserid, true);
    }
    else if (old == ::mega::MegaUser::VISIBILITY_HIDDEN && newVisibility == ::mega::MegaUser::VISIBILITY_VISIBLE)
    {
        mClist.client.presenced().addPeer(mUserid);
        if (mChatRoom)
            mChatRoom->notifyRejoinedChat();
    }
}

void ContactList::syncWithApi(mega::MegaUserList& users)
{
    std::set<uint64_t> apiUsers;
    auto size = users.size();
    for (int i = 0; i < size; i++)
    {
        auto& user = *users.get(i);
        if (user.getVisibility() == ::mega::MegaUser::VISIBILITY_UNKNOWN)   // user cancelled the account in MEGA
        {
            continue;
        }
        apiUsers.insert(user.getHandle());
        addUserFromApi(user);
    }

    // finally, remove known users that are not anymore in the list of users reported by the SDK
    for (auto it = begin(); it != end();)
    {
        auto handle = it->first;
        if (apiUsers.find(handle) != apiUsers.end())
        {
            it++;
            continue;
        }

        auto erased = it;
        it++;
        removeUser(erased);
    }
}

void ContactList::onUserAddRemove(mega::MegaUser& user)
{
    if (user.getVisibility() == ::mega::MegaUser::VISIBILITY_INACTIVE)
    {
        auto it = this->find(user.getHandle());
        if (it != this->end())
        {
            removeUser(it);
        }
    }
    else
    {
        addUserFromApi(user);
    }
}

void ContactList::removeUser(iterator it)
{
    auto handle = it->first;
    delete it->second;
    erase(it);
    client.db.query("delete from contacts where userid=?", handle);
}
void ContactList::onPresenceChanged(Id userid, Presence pres)
{
    auto it = find(userid);
    if (it == end())
        return;
    {
        it->second->updatePresence(pres);
    }
}
void ContactList::setAllOffline()
{
    for (auto& it: *this)
    {
        it.second->updatePresence(Presence::kOffline);
    }
}

promise::Promise<void> ContactList::removeContactFromServer(uint64_t userid)
{
    auto it = find(userid);
    if (it == end())
        return promise::Error("User "+karere::Id(userid).toString()+" not in contactlist");

    auto& api = client.api;
    std::unique_ptr<mega::MegaUser> user(api.sdk.getContact(it->second->email().c_str()));
    if (!user)
        return promise::Error("Could not get user object from email");
    //we don't remove it, we just set visibility to HIDDEN
    return api.callIgnoreResult(&::mega::MegaApi::removeContact, user.get());
}

ContactList::~ContactList()
{
    for (auto& it: *this)
        delete it.second;
}

const std::string* ContactList::getUserEmail(uint64_t userid) const
{
    auto it = find(userid);
    if (it == end())
        return nullptr;
    return &(it->second->email());
}

bool ContactList::isExContact(Id userid)
{
    auto it = find(userid);
    if (it == end() || (it != end() && it->second->visibility() != mega::MegaUser::VISIBILITY_HIDDEN))
    {
        return false;
    }

    return true;
}

Contact* ContactList::contactFromEmail(const std::string &email) const
{
    for (auto it = begin(); it != end(); it++)
    {
        if (it->second->email() == email)
        {
            return it->second;
        }
    }
    return nullptr;
}

Contact* ContactList::contactFromUserId(uint64_t userid) const
{
    auto it = find(userid);
    return (it == end())? nullptr : it->second;
}

Contact::Contact(ContactList& clist, const uint64_t& userid,
                 const std::string& email, int visibility,
                 int64_t since, PeerChatRoom* room)
    :mClist(clist), mUserid(userid), mChatRoom(room), mEmail(email),
     mSince(since), mVisibility(visibility)
{
    auto appClist = clist.client.app.contactListHandler();
    mDisplay = appClist ? appClist->addContactItem(*this) : nullptr;

    mUsernameAttrCbId = mClist.client.userAttrCache().getAttr(userid,
        USER_ATTR_FULLNAME, this,
        [](Buffer* data, void* userp)
        {
            //even if both first and last name are null, the data is at least
            //one byte - the firstname-size-prefix, which will be zero
            auto self = static_cast<Contact*>(userp);
            if (!data || data->empty() || (*data->buf() == 0))
                self->updateTitle(encodeFirstName(self->mEmail));
            else
                self->updateTitle(std::string(data->buf(), data->dataSize()));
        });
    if (mTitleString.empty()) // user attrib fetch was not synchornous
    {
        updateTitle(encodeFirstName(email));
        assert(!mTitleString.empty());
    }
    auto& client = mClist.client;
    if ((client.initState() >= Client::kInitHasOnlineSession)
         && (mVisibility == ::mega::MegaUser::VISIBILITY_VISIBLE))
        client.presenced().addPeer(mUserid);

    mIsInitializing = false;
}

// the title string starts with a byte equal to the first name length, followed by first name,
// then second name
void Contact::updateTitle(const std::string& str)
{
    mTitleString = str;
    notifyTitleChanged();
}

void Contact::notifyTitleChanged()
{
    callAfterInit(this, [this]
    {
        if (mDisplay)
            mDisplay->onTitleChanged(mTitleString);

        //1on1 chatrooms don't have a binary layout for the title
        if (mChatRoom)
            mChatRoom->updateTitle(mTitleString.substr(1));
    }, mClist.client.appCtx);
}

Contact::~Contact()
{
    auto& client = mClist.client;
    if (!client.isTerminated())
    {
        client.userAttrCache().removeCb(mUsernameAttrCbId);
        client.presenced().removePeer(mUserid, true);
        if (mDisplay)
        {
            client.app.contactListHandler()->removeContactItem(*mDisplay);
        }
    }
}

promise::Promise<ChatRoom*> Contact::createChatRoom()
{
    if (mChatRoom)
    {
        KR_LOG_WARNING("Contact::createChatRoom: chat room already exists, check before calling this method");
        return Promise<ChatRoom*>(mChatRoom);
    }
    mega::MegaTextChatPeerListPrivate peers;
    peers.addPeer(mUserid, chatd::PRIV_OPER);
    return mClist.client.api.call(&mega::MegaApi::createChat, false, &peers, nullptr)
    .then([this](ReqResult result) -> Promise<ChatRoom*>
    {
        auto& list = *result->getMegaTextChatList();
        if (list.size() < 1)
            return promise::Error("Empty chat list returned from API");
        auto room = mClist.client.chats->addRoom(*list.get(0));
        if (!room)
            return promise::Error("API created an incorrect 1on1 room");
        room->connect();
        return room;
    });
}

void Contact::setChatRoom(PeerChatRoom& room)
{
    assert(!mChatRoom);
    assert(!mTitleString.empty());
    mChatRoom = &room;
    mChatRoom->updateTitle(mTitleString.substr(1));
}

void Contact::attachChatRoom(PeerChatRoom& room)
{
    if (mChatRoom)
        throw std::runtime_error("attachChatRoom[room "+Id(room.chatid()).toString()+ "]: contact "+
            Id(mUserid).toString()+" already has a chat room attached");
    KR_LOG_DEBUG("Attaching 1on1 chatroom %s to contact %s", ID_CSTR(room.chatid()), ID_CSTR(mUserid));
    setChatRoom(room);
}

#define RETURN_ENUM_NAME(name) case name: return #name

const char* Client::initStateToStr(unsigned char state)
{
    switch (state)
    {
        RETURN_ENUM_NAME(kInitCreated);
        RETURN_ENUM_NAME(kInitWaitingNewSession);
        RETURN_ENUM_NAME(kInitHasOfflineSession);
        RETURN_ENUM_NAME(kInitHasOnlineSession);
        RETURN_ENUM_NAME(kInitTerminated);
        RETURN_ENUM_NAME(kInitErrGeneric);
        RETURN_ENUM_NAME(kInitErrNoCache);
        RETURN_ENUM_NAME(kInitErrCorruptCache);
        RETURN_ENUM_NAME(kInitErrSidMismatch);
        RETURN_ENUM_NAME(kInitErrSidInvalid);
    default:
        return "(unknown)";
    }
}
const char* Client::connStateToStr(ConnState state)
{
    switch(state)
    {
        RETURN_ENUM_NAME(kDisconnected);
        RETURN_ENUM_NAME(kConnecting);
        RETURN_ENUM_NAME(kConnected);
        default: return "(invalid)";
    }
}

bool Client::isCallInProgress(Id chatid) const
{
    bool callInProgress = false;

#ifndef KARERE_DISABLE_WEBRTC
    if (rtc)
    {
        callInProgress = rtc->isCallInProgress(chatid);
    }
#endif

    return callInProgress;
}

#ifndef KARERE_DISABLE_WEBRTC
rtcModule::ICallHandler* Client::onCallIncoming(rtcModule::ICall& call, karere::AvFlags av)
{
    return app.onIncomingCall(call, av);
}
#endif

std::string encodeFirstName(const std::string& first)
{
    std::string result;
    result.reserve(first.size()+1);
    result+=(char)(first.size());
    if (!first.empty())
    {
        result.append(first);
    }
    return result;
}

}
