GateOpener service
==================

GateOpener service allows to pass full browser checks that some websites require.
It uses a full Firefox browser in order to work.

FixBrowser/FixProxy connects to this service when it encounters a check requiring
a full browser. It passes the request to the service and waits for the result (typically
a set of cookies) while proxying all the internet communication through FixBrowser/FixProxy.

You only need to install this if you're a developer or want to self-host this service.
Regular users can get access to a public service by donating to the project.

Requirements:
- Linux machine or VM
- Firefox ESR 128
- VNC server or similar X11-based setup to run it in "headless" mode

To build you need to run compilation script:

  compile.sh (for Linux)

Once compiled copy the gateopener.json-dist config file to gateopener.json
and edit it for your needs.

Description of the options:

  localhost          - disable to listen on all interfaces
  port               - port for the service
  socks_port_base    - initial port for per-instance internal SOCKS5 proxy
  bidi_port_base     - initial port for per-instance WebDriver BiDi protocol
  profile_template   - path to Firefox profile directory
  work_dir           - path to temporary location where profiles will be copied to
  firefox_binary     - name of Firefox executable
  firefox_parameters - list of additional Firefox parameters
  firefox_start_wait - number of seconds to wait after starting Firefox process
  firefox_kill_wait  - number of seconds to wait after killing Firefox process
  num_instances      - number of Firefox instances
  request_time_limit - time limit in seconds for receiving the request
  session_time_limit - time limit in seconds for the whole session
  viewport_origin    - screen coordinates of viewport in Firefox
  blocked_domains    - list of blocked domains (includes subdomains)

To use put a gateopener.json in FixBrowser/FixProxy location with this content:

{
  "server": "localhost",
  "port": 12345
}
