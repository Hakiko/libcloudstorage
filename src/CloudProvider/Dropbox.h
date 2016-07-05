/*****************************************************************************
 * Dropbox.h : prototypes for Dropbox
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

#ifndef DROPBOX_H
#define DROPBOX_H

#include "CloudProvider.h"

namespace cloudstorage {

class Dropbox : public CloudProvider {
 public:
  Dropbox();

  std::string name() const;
  IItem::Pointer rootDirectory() const;

  GetItemDataRequest::Pointer getItemDataAsync(
      IItem::Pointer, std::function<void(IItem::Pointer)> f);

  HttpRequest::Pointer listDirectoryRequest(const IItem&,
                                            std::ostream& input_stream) const;
  HttpRequest::Pointer uploadFileRequest(const IItem& directory,
                                         const std::string& filename,
                                         std::istream& stream,
                                         std::ostream& input_stream) const;
  HttpRequest::Pointer downloadFileRequest(const IItem&,
                                           std::ostream& input_stream) const;
  HttpRequest::Pointer getThumbnailRequest(const IItem&,
                                           std::ostream& input_stream) const;

  std::vector<IItem::Pointer> listDirectoryResponse(
      std::istream&, HttpRequest::Pointer& next_page_request,
      std::ostream& next_page_request_input) const;

 private:
  class Auth : public cloudstorage::Auth {
   public:
    Auth();

    std::string authorizeLibraryUrl() const;
    Token::Pointer fromTokenString(const std::string&) const;

    HttpRequest::Pointer exchangeAuthorizationCodeRequest(
        std::ostream& input_data) const;
    HttpRequest::Pointer refreshTokenRequest(std::ostream& input_data) const;
    HttpRequest::Pointer validateTokenRequest(std::ostream& input_data) const;

    Token::Pointer exchangeAuthorizationCodeResponse(std::istream&) const;
    Token::Pointer refreshTokenResponse(std::istream&) const;
    bool validateTokenResponse(std::istream&) const;
  };

  class DataRequest : public GetItemDataRequest {
   public:
    DataRequest(CloudProvider::Pointer, IItem::Pointer item,
                std::function<void(IItem::Pointer)> callback);

    void finish();
    IItem::Pointer result();

   private:
    int makeTemporaryLinkRequest();
    int makeThumbnailRequest();

    std::shared_future<IItem::Pointer> result_;
    IItem::Pointer item_;
  };
};

}  // namespace cloudstorage

#endif  // DROPBOX_H
