// X509LS
// Copyright 2013 Tom Harwood

#include "x509ls/cli/certificate_list_layout.h"

#include <ctype.h>
#include <ncurses.h>

#include <sstream>

#include "x509ls/cli/base/cli_application.h"
#include "x509ls/cli/base/colours.h"
#include "x509ls/cli/base/command_line.h"
#include "x509ls/cli/base/list_control.h"
#include "x509ls/cli/base/text_control.h"
#include "x509ls/cli/certificate_list_control.h"
#include "x509ls/cli/certificate_view_layout.h"
#include "x509ls/cli/menu_bar.h"
#include "x509ls/cli/status_bar.h"
#include "x509ls/net/chain_fetcher.h"
#include "x509ls/net/ssl_client.h"

namespace x509ls {
// static
const char* CertificateListLayout::kMenuText = ""
  "q:quit g:goto-host r:reload t:toggle-display ";
//  "z:settings";

// static
const int CertificateListLayout::kListControlIndexVerificationPath = 0;

// static
const int CertificateListLayout::kListControlIndexPeerChain = 1;

CertificateListLayout::CertificateListLayout(CliApplication* application,
    TrustStore* trust_store)
  :
    CliControl(application),
    trust_store_(trust_store),
    menu_bar_(new MenuBar(this, kMenuText)),
    top_status_bar_(new StatusBar(this, "")),
    text_control_(new TextControl(this, "")),
    bottom_status_bar_(new StatusBar(this, "")),
    command_line_(new CommandLine(this)),
    displayed_list_control_index_(kListControlIndexVerificationPath),
    address_family_(DnsLookup::kAddressFamilyIPv4then6),
    tls_method_index_(0),
    tls_auth_type_index_(0),
    current_fetcher_(NULL) {
  list_controls_[kListControlIndexVerificationPath] =
        new CertificateListControl(
          this,
          CertificateListControl::kTypeVerificationPath);

  list_controls_[kListControlIndexPeerChain] =
        new CertificateListControl(
          this,
          CertificateListControl::kTypePeerChain),
  Subscribe(list_controls_[kListControlIndexVerificationPath],
      ListControl::kEventSelectedItemChanged);

  Subscribe(list_controls_[kListControlIndexPeerChain],
      ListControl::kEventSelectedItemChanged);

  AddChild(menu_bar_);
  AddChild(list_controls_[kListControlIndexVerificationPath]);
  AddChild(top_status_bar_);
  AddChild(text_control_);
  AddChild(bottom_status_bar_);
  AddChild(command_line_);

  Subscribe(command_line_, CommandLine::kEventInputAccepted);
  Subscribe(command_line_, CommandLine::kEventInputCancelled);

  UpdateStatusBarOptionsText();

  ShowGotoHostPrompt();
}

// virtual
CertificateListLayout::~CertificateListLayout() {
  if (current_fetcher_ != NULL) {
    current_fetcher_->Cancel();
  }
}

// virtual
bool CertificateListLayout::KeyPressEvent(int keypress) {
  bool handled = false;
  switch (keypress) {
  case 'q':
    handled = true;
    GetApplication()->Close(this);
    break;
  case 'g':
    handled = true;
    ShowGotoHostPrompt();
    break;
  case 'a':
    tls_auth_type_index_ = SslClient::NextAuthType(tls_auth_type_index_);
    UpdateStatusBarOptionsText();
    break;
  case 'm':
    tls_method_index_ = SslClient::NextTlsMethod(tls_method_index_);
    UpdateStatusBarOptionsText();
    break;
  case 'v':
    address_family_ = DnsLookup::NextAddressFamily(address_family_);
    UpdateStatusBarOptionsText();
    handled = true;
    break;
  case 'r':
    if (user_input_hostname_.size() > 0) {
      GotoHost(user_input_hostname_);
    }
    handled = true;
    break;
  case 't':
    ToggleDisplayedListControl();
    break;
  case KEY_UP:
    list_controls_[displayed_list_control_index_]->SelectPrevious();
    handled = true;
    break;
  case KEY_DOWN:
    list_controls_[displayed_list_control_index_]->SelectNext();
    handled = true;
    break;
  case '\n':
  case KEY_ENTER:
    ShowCertificateViewLayout();
    handled = true;
    break;
  default:
    handled = text_control_->OnKeyPress(keypress);
    break;
  }

  return handled;
}

// virtual
void CertificateListLayout::OnEvent(const BaseObject* source, int event_code) {
  if (source == command_line_) {
    switch (event_code) {
    case CommandLine::kEventInputCancelled:
      command_line_->Clear();
      break;
    case CommandLine::kEventInputAccepted:
      switch (current_text_input_type_) {
      case kTextInputTypeGo:
        GotoHost(command_line_->InputText());
        break;
      case kTextInputTypeNone:
        break;
      }
      break;
    }

    SetFocusedChild(NULL);
  } else if (source == current_fetcher_) {
    switch (event_code) {
    case ChainFetcher::kStateResolveFail:
      command_line_->DisplayMessage(current_fetcher_->ErrorMessage());
      break;
    case ChainFetcher::kStateConnecting:
      DisplayLoadingMessage();
      break;
    case ChainFetcher::kStateConnectSuccess:
      DisplayConnectSuccessMessage();
      list_controls_[kListControlIndexVerificationPath]->SetModel(
          current_fetcher_->Path());
      list_controls_[kListControlIndexPeerChain]->SetModel(
          current_fetcher_->Chain());
      bottom_status_bar_->SetMainText(current_fetcher_->VerifyStatus());
      // Emitted ListControl::kSelectedItemChanged event used to update
      // certificate preview text.
      break;
    case ChainFetcher::kStateConnectFail:
      DisplayConnectFailedMessage();
      break;
    default:
      break;
    }
  } else if (source == list_controls_[displayed_list_control_index_]) {
    if (event_code == ListControl::kEventSelectedItemChanged) {
      UpdateDisplayedCertificate();
    }
  }
}

void CertificateListLayout::GotoHost(const string& user_input_hostname) {
  user_input_hostname_ = user_input_hostname;

  if (current_fetcher_ != NULL) {
    current_fetcher_->Cancel();
    Unsubscribe(current_fetcher_);

    list_controls_[kListControlIndexVerificationPath]->SetModel(NULL);
    list_controls_[kListControlIndexPeerChain]->SetModel(NULL);
    UpdateDisplayedCertificate();

    bottom_status_bar_->SetMainText("");

    DeleteChild(current_fetcher_);
  }

  current_fetcher_ = new ChainFetcher(this, trust_store_, user_input_hostname_,
      443, address_family_, tls_method_index_, tls_auth_type_index_);
  Subscribe(current_fetcher_, ChainFetcher::kStateResolving);
  Subscribe(current_fetcher_, ChainFetcher::kStateResolveFail);
  Subscribe(current_fetcher_, ChainFetcher::kStateConnecting);
  Subscribe(current_fetcher_, ChainFetcher::kStateConnectSuccess);
  Subscribe(current_fetcher_, ChainFetcher::kStateConnectFail);
  Subscribe(current_fetcher_, ChainFetcher::kStateResolving);
  current_fetcher_->Start();

  DisplayLoadingMessage();
}

void CertificateListLayout::DisplayLoadingMessage() {
  std::stringstream message;
  message << "Loading " << LocationText();

  command_line_->DisplayMessage(message.str());
}

void CertificateListLayout::DisplayConnectSuccessMessage() {
  std::stringstream message;
  message << "Connected to " << LocationText() << " ok";

  command_line_->DisplayMessage(message.str());
}

void CertificateListLayout::DisplayConnectFailedMessage() {
  std::stringstream message;
  message << "Connection failed to " << LocationText();

  command_line_->DisplayMessage(message.str());
}


string CertificateListLayout::LocationText() const {
  std::stringstream message;
  message << user_input_hostname_;

  if (current_fetcher_->IPAddress().size() > 0) {
    message << " (";
    message << current_fetcher_->IPAddress();
    message << ")";
  }

  return message.str();
}

void CertificateListLayout::UpdateStatusBarOptionsText() {
  std::stringstream options_text;

  options_text << "a:";
  options_text << SslClient::TlsAuthTypeName(tls_auth_type_index_);

  options_text << " m:";
  options_text << SslClient::TlsMethodName(tls_method_index_);

  options_text << " v:";
  options_text << DnsLookup::AddressFamilyName(address_family_);

  options_text << " ";

  bottom_status_bar_->SetExtraText(options_text.str());
}

void CertificateListLayout::ShowGotoHostPrompt() {
  current_text_input_type_ = kTextInputTypeGo;
  command_line_->DisplayPrompt("Goto host: ");
  SetFocusedChild(command_line_);
}

void CertificateListLayout::ToggleDisplayedListControl() {
  displayed_list_control_index_++;
  if (displayed_list_control_index_ > 1) {
    displayed_list_control_index_ = 0;
  }

  ReplaceChild(1, list_controls_[displayed_list_control_index_]);

  UpdateDisplayedCertificate();
}

void CertificateListLayout::UpdateDisplayedCertificate() {
  const Certificate* certificate =
    list_controls_[displayed_list_control_index_]->CurrentCertificate();

  text_control_->SetText(certificate ? certificate->TextDescription() : "");
}

void CertificateListLayout::ShowCertificateViewLayout() {
  const Certificate* certificate =
    list_controls_[displayed_list_control_index_]->CurrentCertificate();

  if (certificate == NULL) {
    return;
  }

  GetApplication()->Show(
      new CertificateViewLayout(GetApplication(), *certificate));
}
}  // namespace x509ls
