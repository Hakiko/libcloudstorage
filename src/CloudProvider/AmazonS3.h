/*****************************************************************************
 * AmazonS3.h : AmazonS3 headers
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef AMAZONS3_H
#define AMAZONS3_H

#include "CloudProvider.h"

#include "Utility/Item.h"

namespace cloudstorage {

/**
 * AmazonS3 requires computing HMAC-SHA256 hashes, so it requires a valid
 * ICrypto implementation. Be careful about renaming and moving directories,
 * because there has to be an http request per each of its subelement. Buckets
 * are listed as root directory's children, renaming and moving them doesn't
 * work. Also, only buckets created with specified region(aws_region hint) are
 * working.
 */
class AmazonS3 : public CloudProvider {
 public:
  AmazonS3();

  void initialize(InitData&& data);

  std::string token() const;
  std::string name() const;
  Hints hints() const;

  AuthorizeRequest::Pointer authorizeAsync();
  GetItemDataRequest::Pointer getItemDataAsync(const std::string& id,
                                               GetItemDataCallback f);
  MoveItemRequest::Pointer moveItemAsync(IItem::Pointer source,
                                         IItem::Pointer destination,
                                         MoveItemCallback);
  RenameItemRequest::Pointer renameItemAsync(IItem::Pointer item,
                                             const std::string&,
                                             RenameItemCallback);
  CreateDirectoryRequest::Pointer createDirectoryAsync(IItem::Pointer parent,
                                                       const std::string& name,
                                                       CreateDirectoryCallback);
  DeleteItemRequest::Pointer deleteItemAsync(IItem::Pointer,
                                             DeleteItemCallback);

  IHttpRequest::Pointer listDirectoryRequest(const IItem&,
                                             const std::string& page_token,
                                             std::ostream& input_stream) const;
  IHttpRequest::Pointer uploadFileRequest(const IItem& directory,
                                          const std::string& filename,
                                          std::ostream& prefix_stream,
                                          std::ostream& suffix_stream) const;
  IHttpRequest::Pointer downloadFileRequest(const IItem&,
                                            std::ostream& input_stream) const;

  std::vector<IItem::Pointer> listDirectoryResponse(
      std::istream&, std::string& next_page_token) const;

  void authorizeRequest(IHttpRequest&) const;
  bool reauthorize(int) const;

  std::string access_id() const;
  std::string secret() const;

  static std::pair<std::string, std::string> split(const std::string&);

  class Auth : public cloudstorage::Auth {
   public:
    Auth();

    std::string authorizeLibraryUrl() const;

    IHttpRequest::Pointer exchangeAuthorizationCodeRequest(
        std::ostream& input_data) const;
    IHttpRequest::Pointer refreshTokenRequest(std::ostream& input_data) const;

    Token::Pointer exchangeAuthorizationCodeResponse(std::istream&) const;
    Token::Pointer refreshTokenResponse(std::istream&) const;
  };

 private:
  std::string access_id_;
  std::string secret_;
  std::string region_;
};

}  // namespace cloudstorage

#endif  // AMAZONS3_H