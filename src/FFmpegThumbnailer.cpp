#include "FFmpegThumbnailer.h"

#include <cloudstorage/IHttp.h>
#include <libffmpegthumbnailer/videothumbnailer.h>
#include <iostream>

EitherError<std::vector<char>> FFmpegThumbnailer::generateThumbnail(
    IItem::Pointer item) {
  if ((item->type() != IItem::FileType::Image &&
       item->type() != IItem::FileType::Video) ||
      item->url().empty()) {
    Error e{IHttpRequest::Failure,
            "can generate thumbnails only for images and videos"};
    return e;
  }
  std::vector<uint8_t> buffer;
  ffmpegthumbnailer::VideoThumbnailer thumbnailer;
  try {
    std::string url = item->url() + "&nocache=true";
    std::cerr << "generating " << url << "\n";
    thumbnailer.generateThumbnail(url, ThumbnailerImageType::Png, buffer);
    auto ptr = reinterpret_cast<const char*>(buffer.data());
    return std::vector<char>(ptr, ptr + buffer.size());
  } catch (const std::exception& e) {
    return cloudstorage::Error{cloudstorage::IHttpRequest::Failure, e.what()};
  }
}
