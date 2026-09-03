#include "attachments.h"
#include "config.h"
#include "image_utils.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QSaveFile>
#include <QRegularExpression>
#include <QJsonArray>

static const qint64 kMaxSourceBytes = 25LL * 1024 * 1024;
static const qint64 kMaxPixels = 40LL * 1000 * 1000;
static QString root() { return pengyConfigDirPath() + "/attachments"; }
bool attachmentIdIsValid(const QString& id) { static QRegularExpression re("^sha256:[0-9a-f]{64}$"); return re.match(id).hasMatch(); }
static QString digest(const QString& id) { return attachmentIdIsValid(id) ? id.mid(7) : QString(); }
QString attachmentObjectPath(const QString& id) { QString d=digest(id); return d.isEmpty() ? QString() : root()+"/objects/sha256/"+d.left(2)+"/"+d; }
QString attachmentDerivativePath(const QString& id,const QString& name) { if (name!="image-display-v1.jpg"&&name!="thumbnail-256-v1.jpg") return {}; QString d=digest(id); return d.isEmpty()?QString():root()+"/derivatives/sha256/"+d.left(2)+"/"+d+"/"+name; }
static bool writeAtomic(const QString& path,const QByteArray& data) { QFileInfo fi(path); QDir().mkpath(fi.dir().absolutePath()); QSaveFile out(path); if(!out.open(QIODevice::WriteOnly)) return false; if(out.write(data)!=data.size()) return false; return out.commit(); }
static QString mimeFor(const QImageReader& r) { QByteArray f=r.format().toLower(); if(f=="png")return "image/png";if(f=="jpeg"||f=="jpg")return "image/jpeg";if(f=="gif")return "image/gif";if(f=="webp")return "image/webp";if(f=="bmp")return "image/bmp";if(f=="tiff")return "image/tiff";return "application/octet-stream"; }
QJsonObject attachmentImportImage(const QString& sourcePath,const QString& displayName,int maxDimension,double maxMb,int quality) {
 QFile f(sourcePath); if(!f.open(QIODevice::ReadOnly)||f.size()<=0||f.size()>kMaxSourceBytes)return {}; QByteArray bytes=f.readAll(); f.close(); QImageReader reader(sourcePath); reader.setAutoTransform(true); QSize size=reader.size(); if(!size.isValid()||(qint64)size.width()*size.height()>kMaxPixels)return {}; QImage probe=reader.read();if(probe.isNull())return {}; QString hex=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());QString id="sha256:"+hex;QString object=attachmentObjectPath(id);if(!QFile::exists(object)&&!writeAtomic(object,bytes))return {};if(!attachmentEnsureImageDerivatives(id,maxDimension,maxMb,quality))return {};QJsonObject ref{{"v",1},{"id",id},{"kind","image"},{"name",displayName.simplified().left(240)},{"media_type",mimeFor(reader)},{"byte_size",(double)bytes.size()},{"created_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"image",QJsonObject{{"width",probe.width()},{"height",probe.height()}}}};return ref;
}
bool attachmentEnsureImageDerivatives(const QString& id,int maxDimension,double maxMb,int quality) { QString display=attachmentDerivativePath(id,"image-display-v1.jpg"),thumb=attachmentDerivativePath(id,"thumbnail-256-v1.jpg");if(display.isEmpty()||!QFile::exists(attachmentObjectPath(id)))return false;if(QFile::exists(display)&&QFile::exists(thumb))return true;ImageResult source=imagePreprocess(attachmentObjectPath(id),maxDimension,maxMb,quality);if(!source.ok)return false;QImage img;img.loadFromData(QByteArray::fromBase64(source.bytes_base64));if(img.isNull())return false;img=img.convertToFormat(QImage::Format_RGB888);QByteArray d;QBuffer db(&d);db.open(QIODevice::WriteOnly);img.save(&db,"JPEG",quality);QImage small=img.scaled(256,256,Qt::KeepAspectRatio,Qt::SmoothTransformation);QByteArray t;QBuffer tb(&t);tb.open(QIODevice::WriteOnly);small.save(&tb,"JPEG",82);return (QFile::exists(display)||writeAtomic(display,d))&&(QFile::exists(thumb)||writeAtomic(thumb,t)); }
QString attachmentImageDataUrl(const QJsonObject& ref,int maxDimension,double maxMb,int quality) { if(ref["kind"].toString()!="image")return {};QString id=ref["id"].toString();if(!attachmentEnsureImageDerivatives(id,maxDimension,maxMb,quality))return {};QFile f(attachmentDerivativePath(id,"image-display-v1.jpg"));if(!f.open(QIODevice::ReadOnly))return {};return "data:image/jpeg;base64,"+QString::fromLatin1(f.readAll().toBase64()); }
QString attachmentLabel(const QJsonObject& ref) { if(ref["kind"].toString()=="image"){QJsonObject i=ref["image"].toObject();QString d=i.isEmpty()?QString():QString(" · %1×%2").arg(i["width"].toInt()).arg(i["height"].toInt());return "[image: "+ref["name"].toString("Image")+d+"]";}return "[attachment: "+ref["name"].toString(ref["kind"].toString("unknown"))+"]"; }
QJsonObject attachmentStorageReport(const QJsonArray& chats) {
 QSet<QString> refs; for (const auto& cv : chats) for (const auto& mv : cv.toObject()["messages"].toArray()) for (const auto& rv : mv.toObject()["attachments"].toArray()) refs.insert(rv.toObject()["id"].toString());
 quint64 objects=0, bytes=0, reclaimable=0; QDir base(root()+"/objects/sha256");
 for (const auto& prefix : base.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) for (const auto& name : QDir(base.filePath(prefix)).entryList(QDir::Files)) {
  if (name.size()!=64 || !QRegularExpression("^[0-9a-f]{64}$").match(name).hasMatch()) continue; quint64 size=QFileInfo(base.filePath(prefix+"/"+name)).size(); ++objects; bytes+=size; if(!refs.contains("sha256:"+name)) reclaimable+=size;
 }
 return QJsonObject{{"objects",(double)objects},{"object_bytes",(double)bytes},{"referenced",refs.size()},{"reclaimable_bytes",(double)reclaimable},{"delete_performed",false}};
}
