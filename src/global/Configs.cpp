#include "include/global/Configs.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QProcess>
#include <QDebug>
#include <utility>
#include <include/api/RPC.h>

#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/RoutesRepo.h"


#ifdef Q_OS_WIN
#include "include/sys/windows/guihelper.h"
#else
#ifdef Q_OS_LINUX
#include <include/sys/linux/LinuxCap.h>
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

    // System Utils
namespace Configs {
    static QJsonObject cleanJsonObjectMigration(const QJsonObject& obj) {
        QJsonObject result;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isObject()) {
                QJsonObject cleaned = cleanJsonObjectMigration(it.value().toObject());
                if (!cleaned.isEmpty()) result[it.key()] = cleaned;
            } else if (it.value().isArray()) {
                if (!it.value().toArray().isEmpty()) result[it.key()] = it.value();
            } else if (it.value().isString()) {
                if (!it.value().toString().isEmpty()) result[it.key()] = it.value();
            } else if (it.value().isBool()) {
                if (it.value().toBool()) result[it.key()] = it.value();
            } else if (it.value().isDouble()) {
                if (it.value().toDouble() != 0) result[it.key()] = it.value();
            }
        }
        return result;
    }

    bool maybeMigrate(const std::string& dbPath) {
        QString path = QString::fromStdString(dbPath);
        if (QFile::exists(path)) return false;

        QString configDir = QFileInfo(path).absolutePath();
        if (!QFile::exists(configDir + "/configs.json")) return false;

        qDebug() << "Legacy configuration detected. Migrating to SQLite...";

        {
            DatabaseManager dm(dbPath);
            Database& db = dm.getDatabase();

            // 1. Settings
            QFile settingsFile(configDir + "/configs.json");
            if (settingsFile.open(QIODevice::ReadOnly)) {
                QJsonObject configs = QJsonDocument::fromJson(settingsFile.readAll()).object();
                for (auto it = configs.begin(); it != configs.end(); ++it) {
                    QString val = it.value().isBool() ? (it.value().toBool() ? "true" : "false") : it.value().toVariant().toString();
                    db.exec("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
                            it.key().toStdString(), val.toStdString());
                }
            }

            // 2. Groups
            QDir groupsDir(configDir + "/groups");
            QStringList groupFiles = groupsDir.entryList({"*.json"}, QDir::Files);
            for (const QString& fileName : groupFiles) {
                if (fileName == "pm.json") continue;
                QFile f(groupsDir.absoluteFilePath(fileName));
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonObject gJson = QJsonDocument::fromJson(f.readAll()).object();
                    auto group = GroupsRepo::NewGroup();
                    group->id = gJson["id"].toInt();
                    group->archive = gJson["archive"].toBool();
                    group->skip_auto_update = gJson["skip_auto_update"].toBool();
                    group->name = gJson["name"].toString();
                    group->url = gJson["url"].toString();
                    group->info = gJson["info"].toString();
                    group->sub_last_update = (qint64)gJson["lastup"].toDouble();
                    group->front_proxy_id = gJson["front_proxy_id"].toInt();
                    group->landing_proxy_id = gJson["landing_proxy_id"].toInt();
                    group->column_width = QJsonArray2QListInt(gJson["column_width"].toArray());
                    group->profiles = QJsonArray2QListInt(gJson["profiles"].toArray());
                    
                    dm.groupsRepo->Save(group);
                }
            }
            auto queryGOrder = db.query("SELECT id FROM groups ORDER BY id");
            int gOrder = 0;
            while (queryGOrder && queryGOrder->executeStep()) {
                db.exec("INSERT INTO groups_order (group_id, display_order) VALUES (?, ?)",
                        queryGOrder->getColumn(0).getInt(), gOrder++);
            }

            // 3. Profiles
            QDir profilesDir(configDir + "/profiles");
            for (const QString& fileName : profilesDir.entryList({"*.json"}, QDir::Files)) {
                QFile f(profilesDir.absoluteFilePath(fileName));
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonObject pJson = QJsonDocument::fromJson(f.readAll()).object();
                    QString type = pJson["type"].toString();
                    QJsonObject outbound = pJson["outbound"].toObject();

                    if (outbound.contains("dial_fields")) {
                        QJsonObject df = outbound["dial_fields"].toObject();
                        outbound.remove("dial_fields");
                        for (auto it = df.begin(); it != df.end(); ++it) outbound[it.key()] = it.value();
                    }

                    if (type == "hysteria" && outbound["protocol_version"].toString() == "2") {
                        outbound["type"] = "hysteria2";
                        if (!outbound["obfs"].toString().isEmpty()) {
                            outbound["obfs"] = QJsonObject{{"type", "salamander"}, {"password", outbound["obfs"]}};
                        }
                    } else {
                        outbound["type"] = type;
                    }
                    if (outbound.contains("name")) {
                        outbound["tag"] = outbound["name"];
                        outbound.remove("name");
                    }

                    auto profile = ProfilesRepo::NewProfile(type);
                    profile->id = pJson["id"].toInt();
                    profile->gid = pJson["gid"].toInt();
                    profile->latency = pJson["yc"].toInt();
                    profile->dl_speed = pJson["dl"].toString();
                    profile->ul_speed = pJson["ul"].toString();
                    profile->test_country = pJson["country"].toString();
                    profile->ip_out = pJson["report"].toString();
                    
                    profile->outbound->ParseFromJson(outbound);
                    
                    QJsonObject traffic = pJson["traffic"].toObject();
                    profile->traffic_data->ParseFromJson(traffic);
                    
                    dm.profilesRepo->Save(profile);
                }
            }

            // 4. Routes
            QDir routesDir(configDir + "/route_profiles");
            for (const QString& fileName : routesDir.entryList({"*.json"}, QDir::Files)) {
                QFile f(routesDir.absoluteFilePath(fileName));
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonObject rJson = QJsonDocument::fromJson(f.readAll()).object();
                    auto rp = RoutesRepo::NewRouteProfile();
                    rp->id = rJson["id"].toInt();
                    if (rp->id < 0) continue;
                    rp->name = rJson["name"].toString();
                    rp->defaultOutboundID = rJson["default_outbound"].toInt();
                    
                    QJsonArray rules = rJson["rules"].toArray();
                    for (int i = 0; i < rules.size(); ++i) {
                        QJsonObject ruleJson = rules[i].toObject();
                        auto rule = std::make_shared<RouteRule>();
                        rule->name = ruleJson["name"].toString();
                        rule->type = ruleJson["type"].toInt();
                        rule->ip_version = ruleJson["ip_version"].toString();
                        rule->network = ruleJson["network"].toString();
                        rule->protocol = ruleJson["protocol"].toString();
                        rule->inbound = QJsonArray2QListString(ruleJson["inbound"].toArray());
                        rule->domain = QJsonArray2QListString(ruleJson["domain"].toArray());
                        rule->domain_suffix = QJsonArray2QListString(ruleJson["domain_suffix"].toArray());
                        rule->domain_keyword = QJsonArray2QListString(ruleJson["domain_keyword"].toArray());
                        rule->domain_regex = QJsonArray2QListString(ruleJson["domain_regex"].toArray());
                        rule->source_ip_cidr = QJsonArray2QListString(ruleJson["source_ip_cidr"].toArray());
                        rule->source_ip_is_private = ruleJson["source_ip_is_private"].toBool();
                        rule->ip_cidr = QJsonArray2QListString(ruleJson["ip_cidr"].toArray());
                        rule->ip_is_private = ruleJson["ip_is_private"].toBool();
                        rule->source_port = QJsonArray2QListString(ruleJson["source_port"].toArray());
                        rule->source_port_range = QJsonArray2QListString(ruleJson["source_port_range"].toArray());
                        rule->port = QJsonArray2QListString(ruleJson["port"].toArray());
                        rule->port_range = QJsonArray2QListString(ruleJson["port_range"].toArray());
                        rule->process_name = QJsonArray2QListString(ruleJson["process_name"].toArray());
                        rule->process_path = QJsonArray2QListString(ruleJson["process_path"].toArray());
                        rule->process_path_regex = QJsonArray2QListString(ruleJson["process_path_regex"].toArray());
                        rule->rule_set = QJsonArray2QListString(ruleJson["rule_set"].toArray());
                        rule->invert = ruleJson["invert"].toBool();
                        rule->outboundID = ruleJson["outboundID"].toInt();
                        rule->action = ruleJson["actionType"].toString();
                        
                        if (rule->action == "route") {
                            if (rule->outboundID == -3) rule->action = "reject";
                            else if (rule->outboundID == -4) rule->action = "hijack-dns";
                        }
                        
                        rule->rejectMethod = ruleJson["rejectMethod"].toString();
                        rule->no_drop = ruleJson["noDrop"].toBool();
                        rule->override_address = ruleJson["override_address"].toString();
                        rule->override_port = ruleJson["override_port"].toString();
                        rule->sniffers = QJsonArray2QListString(ruleJson["sniffers"].toArray());
                        rule->sniffOverrideDest = ruleJson["sniffOverrideDest"].toBool();
                        rule->strategy = ruleJson["strategy"].toString();
                        
                        rp->Rules.append(rule);
                    }
                    dm.routesRepo->Save(rp);
                }
            }
            
            auto queryPMax = db.query("SELECT MAX(id) FROM profiles");
            if (queryPMax && queryPMax->executeStep()) db.exec("UPDATE entity_ids SET profile_last_id = ?", queryPMax->getColumn(0).getInt());
            auto queryGMax = db.query("SELECT MAX(id) FROM groups");
            if (queryGMax && queryGMax->executeStep()) db.exec("UPDATE entity_ids SET group_last_id = ?", queryGMax->getColumn(0).getInt());
            auto queryRMax = db.query("SELECT MAX(id) FROM route_profiles");
            if (queryRMax && queryRMax->executeStep()) db.exec("UPDATE entity_ids SET route_profile_last_id = ?", queryRMax->getColumn(0).getInt());
        }

        QDir cfgDir(configDir);
        cfgDir.rename("groups", "groups.bak");
        cfgDir.rename("profiles", "profiles.bak");
        if (QFile::exists(configDir + "/route_profiles")) cfgDir.rename("route_profiles", "route_profiles.bak");
        QFile::rename(configDir + "/configs.json", configDir + "/configs.json.bak");

        qDebug() << "Migration successful.";
        return true;
    }

    void initDB(const std::string& dbPath) {
        if (maybeMigrate(dbPath)) {
            QProcess::startDetached(QApplication::applicationFilePath(), QApplication::arguments());
            exit(0);
        }

        dataManager = new DatabaseManager(dbPath);

        if (dataManager->groupsRepo->GetAllGroupIds().empty()) {
            auto defaultGroup = GroupsRepo::NewGroup();
            defaultGroup->name = QObject::tr("Default");
            dataManager->groupsRepo->AddGroup(defaultGroup);
        }
        if (dataManager->routesRepo->GetAllRouteProfileIds().empty()) {
            auto defaultRoute = RouteProfile::GetDefaultChain();
            dataManager->routesRepo->AddRouteProfile(defaultRoute);
        }
    }

    QString FindCoreRealPath() {
        auto fn = QApplication::applicationDirPath() + "/ThroneCore";
#ifdef Q_OS_WIN
        fn += ".exe";
#endif
        auto fi = QFileInfo(fn);
        QString path;
        if (fi.isSymLink()) path =  fi.symLinkTarget();
        path = fn;
#ifdef Q_OS_WIN
        path.replace("/", "\\");
#endif
        return path;
    }

    short isAdminCache = -1;

    bool isSetuidSet(const std::string& path) {
#ifdef Q_OS_MACOS
        struct stat fileInfo;

        if (stat(path.c_str(), &fileInfo) != 0) {
            return false;
        }

        if (fileInfo.st_mode & S_ISUID) {
            return true;
        } else {
            return false;
        }
#else
        return false;
#endif
    }

    // IsAdmin 主要判断：有无权限启动 Tun
    bool IsAdmin(bool forceRenew) {
        if (isAdminCache >= 0 && !forceRenew) return isAdminCache;

        bool admin = false;
#ifdef Q_OS_WIN
        admin = Windows_IsInAdmin();
        Configs::dataManager->settingsRepo->windows_set_admin = admin;
#else
        bool ok;
        auto isPrivileged = API::defaultClient->IsPrivileged(&ok);
        admin = ok && isPrivileged;
#endif
        isAdminCache = admin;
        return admin;
    };

    QString GetBasePath() {
        if (Configs::dataManager->settingsRepo->flag_use_appdata) return QStandardPaths::writableLocation(
              QStandardPaths::AppConfigLocation);
        return qApp->applicationDirPath();
    }
} // namespace Configs
