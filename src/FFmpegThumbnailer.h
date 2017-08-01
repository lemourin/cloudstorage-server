#ifndef FFMPEGTHUMBNAILER_H
#define FFMPEGTHUMBNAILER_H

#include <cloudstorage/IThumbnailer.h>

using namespace cloudstorage;

class FFmpegThumbnailer : public IThumbnailer {
 public:
  EitherError<std::vector<char>> generateThumbnail(
      IItem::Pointer item) override;
};

#endif  // FFMPEGTHUMBNAILER_H
