// Copyright (c) 2012 Intel Corp
// Copyright (c) 2012 The Chromium Authors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell co
// pies of the Software, and to permit persons to whom the Software is furnished
//  to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in al
// l copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IM
// PLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNES
// S FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
//  OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WH
// ETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
//  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "content/nw/src/shell_browser_main_parts.h"

#include "base/bind.h"
#include "base/command_line.h"
#include "base/message_loop/message_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/common/chrome_switches.h"
#include "content/nw/src/api/app/app.h"
#include "content/nw/src/api/dispatcher_host.h"
#include "content/nw/src/breakpad_linux.h"
#include "content/nw/src/browser/printing/print_job_manager.h"
#include "content/nw/src/browser/shell_devtools_delegate.h"
#include "content/nw/src/common/shell_switches.h"
#include "content/nw/src/nw_package.h"
#include "content/nw/src/nw_shell.h"
#include "content/nw/src/shell_browser_context.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/main_function_params.h"
#include "grit/net_resources.h"
#include "net/base/net_module.h"
#include "net/proxy/proxy_resolver_v8.h"
#include "ui/base/resource/resource_bundle.h"

#if !defined(OS_WIN)
#include <sys/resource.h>
#endif

#if defined(TOOLKIT_GTK)
#include "content/nw/src/browser/printing/print_dialog_gtk.h"
#endif

#if defined(OS_MACOSX)
#include "content/browser/renderer_host/compositing_iosurface_shader_programs_mac.h"
#endif

using base::MessageLoop;

namespace {

#if !defined(OS_WIN)
// Sets the file descriptor soft limit to |max_descriptors| or the OS hard
// limit, whichever is lower.
void SetFileDescriptorLimit(unsigned int max_descriptors) {
  struct rlimit limits;
  if (getrlimit(RLIMIT_NOFILE, &limits) == 0) {
    unsigned int new_limit = max_descriptors;
    if (limits.rlim_max > 0 && limits.rlim_max < max_descriptors) {
      new_limit = limits.rlim_max;
    }
    limits.rlim_cur = new_limit;
    if (setrlimit(RLIMIT_NOFILE, &limits) != 0) {
      PLOG(INFO) << "Failed to set file descriptor limit";
    }
  } else {
    PLOG(INFO) << "Failed to get file descriptor limit";
  }
}

#endif

base::StringPiece PlatformResourceProvider(int key) {
  if (key == IDR_DIR_HEADER_HTML) {
    base::StringPiece html_data =
        ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
            IDR_DIR_HEADER_HTML);
    return html_data;
  }
  return base::StringPiece();
}

bool ParseCSSColorString(
    const std::string& color_string,
    SkColor* result) {
  std::string formatted_color = "#";
  // Check the string for incorrect formatting.
  if (color_string[0] != '#')
    return false;

  // Convert the string from #FFF format to #FFFFFF format.
  if (color_string.length() == 4) {
    for (size_t i = 1; i < color_string.length(); i++) {
      formatted_color += color_string[i];
      formatted_color += color_string[i];
    }
  } else {
    formatted_color = color_string;
  }

  if (formatted_color.length() != 7)
    return false;

  // Convert the string to an integer and make sure it is in the correct value
  // range.
  int color_ints[3] = {0};
  for (int i = 0; i < 3; i++) {
    if (!base::HexStringToInt(formatted_color.substr(1 + (2 * i), 2),
                              color_ints + i))
      return false;
    if (color_ints[i] > 255 || color_ints[i] < 0)
      return false;
  }

  *result = SkColorSetARGB(255, color_ints[0], color_ints[1], color_ints[2]);
  return true;
}

}  // namespace

namespace content {

ShellBrowserMainParts::ShellBrowserMainParts(
    const MainFunctionParams& parameters)
    : BrowserMainParts(),
      parameters_(parameters),
      run_message_loop_(true),
      devtools_delegate_(NULL),
      notify_result_(ProcessSingleton::PROCESS_NONE)
{
#if defined(ENABLE_PRINTING)
  // Must be created after the NotificationService.
  print_job_manager_.reset(new printing::PrintJobManager);
#endif
}

ShellBrowserMainParts::~ShellBrowserMainParts() {
}

#if !defined(OS_MACOSX)
void ShellBrowserMainParts::PreMainMessageLoopStart() {
}
#endif

void ShellBrowserMainParts::PreMainMessageLoopRun() {
#if !defined(OS_MACOSX)
  Init();
#endif

  if (parameters_.ui_task) {
    parameters_.ui_task->Run();
    delete parameters_.ui_task;
    run_message_loop_ = false;
  }
}

bool ShellBrowserMainParts::MainMessageLoopRun(int* result_code)  {
  return !run_message_loop_;
}

void ShellBrowserMainParts::PostMainMessageLoopRun() {
  if (devtools_delegate_)
    devtools_delegate_->Stop();
  if (notify_result_ == ProcessSingleton::PROCESS_NONE)
    process_singleton_->Cleanup();

  browser_context_.reset();
  off_the_record_browser_context_.reset();
}

void ShellBrowserMainParts::PostMainMessageLoopStart() {
#if defined(TOOLKIT_GTK)
  printing::PrintingContextGtk::SetCreatePrintDialogFunction(
      &PrintDialogGtk::CreatePrintDialog);
#endif
}

int ShellBrowserMainParts::PreCreateThreads() {
  net::ProxyResolverV8::RememberDefaultIsolate();
  return 0;
}

void ShellBrowserMainParts::Init() {
  package_.reset(new nw::Package());
  CommandLine& command_line = *CommandLine::ForCurrentProcess();

  browser_context_.reset(new ShellBrowserContext(false, package()));
  off_the_record_browser_context_.reset(
      new ShellBrowserContext(true, package()));

  process_singleton_.reset(new ProcessSingleton(browser_context_->GetPath(),
                                                base::Bind(&ShellBrowserMainParts::ProcessSingletonNotificationCallback, base::Unretained(this))));
  notify_result_ = process_singleton_->NotifyOtherProcessOrCreate();


  // Quit if the other existing instance want to handle it.
  if (notify_result_ == ProcessSingleton::PROCESS_NOTIFIED) {
    MessageLoop::current()->PostTask(FROM_HERE, MessageLoop::QuitClosure());
    return;
  }

  net::NetModule::SetResourceProvider(PlatformResourceProvider);

  int port = 0;
  // See if the user specified a port on the command line (useful for
  // automation). If not, use an ephemeral port by specifying 0.

  if (command_line.HasSwitch(switches::kRemoteDebuggingPort)) {
    int temp_port;
    std::string port_str =
        command_line.GetSwitchValueASCII(switches::kRemoteDebuggingPort);
    if (base::StringToInt(port_str, &temp_port) &&
        temp_port > 0 && temp_port < 65535) {
      port = temp_port;
    } else {
      DLOG(WARNING) << "Invalid http debugger port number " << temp_port;
    }
  }
  devtools_delegate_ = new ShellDevToolsDelegate(browser_context_.get(), port);

  Shell::Create(browser_context_.get(),
                package()->GetStartupURL(),
                NULL,
                MSG_ROUTING_NONE,
                NULL);

  std::string str_bg;
  SkColor color;
  package_->root()->GetString(switches::kmInitialBkgd, &str_bg);
  if (ParseCSSColorString(str_bg, &color)) {
    float r = static_cast<float>(SkColorGetR(color)) / 255.0f;
    float g = static_cast<float>(SkColorGetG(color)) / 255.0f;
    float b = static_cast<float>(SkColorGetB(color)) / 255.0f;

    content::CompositingIOSurfaceShaderPrograms::SetBackgroundColor(r, g, b);
  }
}

bool ShellBrowserMainParts::ProcessSingletonNotificationCallback(
    const CommandLine& command_line,
    const base::FilePath& current_directory) {
  if (!package_->self_extract()) {
    // We're in runtime mode, create the new app.
    return false;
  }

  // Don't reuse current instance if 'single-instance' is specified to false.
  bool single_instance;
  if (package_->root()->GetBoolean(switches::kmSingleInstance,
                                   &single_instance) &&
      !single_instance)
    return false;

#if defined(OS_WIN)
  std::string cmd = UTF16ToUTF8(command_line.GetCommandLineString());
#else
  std::string cmd = command_line.GetCommandLineString();
#endif
  static const char* const kSwitchNames[] = {
    switches::kNoSandbox,
    switches::kProcessPerTab,
    switches::kEnableExperimentalWebPlatformFeatures,
    //    switches::kEnableCssShaders,
    switches::kAllowFileAccessFromFiles,
  };
  for (size_t i = 0; i < arraysize(kSwitchNames); ++i) {
    ReplaceSubstringsAfterOffset(&cmd, 0, std::string(" --") + kSwitchNames[i], "");
  }

#if 0
  nwapi::App::EmitOpenEvent(UTF16ToUTF8(cmd));
#else
  nwapi::App::EmitOpenEvent(cmd);
#endif

  return true;
}

printing::PrintJobManager* ShellBrowserMainParts::print_job_manager() {
  // TODO(abarth): DCHECK(CalledOnValidThread());
  // http://code.google.com/p/chromium/issues/detail?id=6828
  // print_job_manager_ is initialized in the constructor and destroyed in the
  // destructor, so it should always be valid.
  DCHECK(print_job_manager_.get());
  return print_job_manager_.get();
}

void ShellBrowserMainParts::PreEarlyInitialization() {
#if !defined(OS_WIN)
  // see chrome_browser_main_posix.cc
  CommandLine& command_line = *CommandLine::ForCurrentProcess();
  const std::string fd_limit_string =
      command_line.GetSwitchValueASCII(switches::kFileDescriptorLimit);
  int fd_limit = 0;
  if (!fd_limit_string.empty()) {
    base::StringToInt(fd_limit_string, &fd_limit);
  }
#if defined(OS_MACOSX)
  // We use quite a few file descriptors for our IPC, and the default limit on
  // the Mac is low (256), so bump it up if there is no explicit override.
  if (fd_limit == 0) {
    fd_limit = 1024;
  }
#endif  // OS_MACOSX
  if (fd_limit > 0)
    SetFileDescriptorLimit(fd_limit);
#endif // !OS_WIN
}

}  // namespace content
