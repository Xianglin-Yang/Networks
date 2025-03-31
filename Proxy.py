# Include the libraries for socket and system calls
import socket
import sys
import os
import argparse
import re
import time

# 1MB buffer size
BUFFER_SIZE = 1000000

# Get the IP address and Port number to use for this web proxy server
parser = argparse.ArgumentParser()
parser.add_argument('hostname', help='the IP Address Of Proxy Server')
parser.add_argument('port', help='the port number of the proxy server')
args = parser.parse_args()
proxyHost = args.hostname
proxyPort = int(args.port)

# Create a server socket, bind it to a port and start listening
try:
  # Create a server socket
  # ~~~~ INSERT CODE ~~~~
  # Use the following code to create a TCP socket
  serverSocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
  # ~~~~ END CODE INSERT ~~~~
  print ('Created socket')
except:
  print ('Failed to create socket')
  sys.exit()
  
try:
  # Bind the the server socket to a host and port
  # ~~~~ INSERT CODE ~~~~
  serverSocket.bind((proxyHost, proxyPort))
  # ~~~~ END CODE INSERT ~~~~
  print ('Port is bound')
except:
  print('Port is already in use')
  sys.exit()

try:
  # Listen on the server socket
  # ~~~~ INSERT CODE ~~~~
  # Set the maximum number of connections (for example, 5) and start listening
  serverSocket.listen(5)
  # ~~~~ END CODE INSERT ~~~~
  print ('Listening to socket')
except:
  print ('Failed to listen')
  sys.exit()

# continuously accept connections
while True:
  print ('Waiting for connection...')
  clientSocket = None

  # Accept connection from client and store in the clientSocket
  try:
    # ~~~~ INSERT CODE ~~~~
    clientSocket, addr = serverSocket.accept()
    # ~~~~ END CODE INSERT ~~~~
    print ('Received a connection')
  except:
    print ('Failed to accept connection')
    sys.exit()

  # Get HTTP request from client
  # and store it in the variable: message_bytes
  # ~~~~ INSERT CODE ~~~~
  message_bytes = clientSocket.recv(BUFFER_SIZE)
  # ~~~~ END CODE INSERT ~~~~
  message = message_bytes.decode('utf-8')
  print ('Received request:')
  print ('< ' + message)

  # Extract the method, URI and version of the HTTP client request 
  requestParts = message.split()
  method = requestParts[0]
  URI = requestParts[1]
  version = requestParts[2]

  print ('Method:\t\t' + method)
  print ('URI:\t\t' + URI)
  print ('Version:\t' + version)
  print ('')

  # Get the requested resource from URI
  # Remove http protocol from the URI
  URI = re.sub('^(/?)http(s?)://', '', URI, count=1)

  # Remove parent directory changes - security
  URI = URI.replace('/..', '')

  # Split hostname from resource name
  resourceParts = URI.split('/', 1)
  hostname = resourceParts[0]
  resource = '/'

  if len(resourceParts) == 2:
    # Resource is absolute URI with hostname and resource
    resource = resource + resourceParts[1]

  print ('Requested Resource:\t' + resource)

  # Check if resource is in cache
  try:
    cacheLocation = './' + hostname + resource
    if cacheLocation.endswith('/'):
        cacheLocation = cacheLocation + 'default'

    print ('Cache location:\t\t' + cacheLocation)

    fileExists = os.path.isfile(cacheLocation)
    
    # Check whether the cache is valid (not expired)
    cache_valid = True
    meta_location = cacheLocation + ".meta"

    if os.path.isfile(meta_location):
      try:
        with open(meta_location, 'r') as meta_file:
          meta_content = meta_file.read()
          expires_match = re.search(r'expires_at: (\d+)', meta_content)
          if expires_match:
            expires_at = int(expires_match.group(1))
            current_time = int(time.time())
              
            # If cache has expired
            if current_time > expires_at:
              print("Cache expired, fetching from origin server")
              cache_valid = False
      except:
        print("Error reading cache metadata, assuming cache is valid")

    # Only use cache if it's valid
    if not fileExists or not cache_valid:
      raise Exception("Cache missing or invalid")
    
    # Check wether the file is currently in the cache
    cacheFile = open(cacheLocation, "r")
    cacheData = cacheFile.readlines()

    print ('Cache hit! Loading from cache file: ' + cacheLocation)
    # ProxyServer finds a cache hit
    # Send back response to client 
    # ~~~~ INSERT CODE ~~~~
    clientSocket.sendall(''.join(cacheData).encode())
    # ~~~~ END CODE INSERT ~~~~
    cacheFile.close()
    print ('Sent to the client:')
    print ('> ' + ''.join(cacheData))
  except:
    # cache miss.  Get resource from origin server
    originServerSocket = None
    # Create a socket to connect to origin server
    # and store in originServerSocket
    # ~~~~ INSERT CODE ~~~~
    originServerSocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # ~~~~ END CODE INSERT ~~~~

    print ('Connecting to:\t\t' + hostname + '\n')
    try:
      # Get the IP address for a hostname
      address = socket.gethostbyname(hostname)
      # Connect to the origin server
      # ~~~~ INSERT CODE ~~~~
      originServerSocket.connect((hostname, 80))  # HTTP default port is 80
      # ~~~~ END CODE INSERT ~~~~
      print ('Connected to origin Server')

      originServerRequest = ''
      originServerRequestHeader = ''
      # Create origin server request line and headers to send
      # and store in originServerRequestHeader and originServerRequest
      # originServerRequest is the first line in the request and
      # originServerRequestHeader is the second line in the request
      # ~~~~ INSERT CODE ~~~~
      originServerRequest = method + " " + resource + " HTTP/1.1"
      originServerRequestHeader = "Host: " + hostname
      # ~~~~ END CODE INSERT ~~~~

      # Construct the request to send to the origin server
      request = originServerRequest + '\r\n' + originServerRequestHeader + '\r\n\r\n'

      # Request the web resource from origin server
      print ('Forwarding request to origin server:')
      for line in request.split('\r\n'):
        print ('> ' + line)

      try:
        originServerSocket.sendall(request.encode())
      except socket.error:
        print ('Forward request to origin failed')
        sys.exit()

      print('Request sent to origin server\n')

      # Get the response from the origin server
      # ~~~~ INSERT CODE ~~~~
      response_bytes = originServerSocket.recv(BUFFER_SIZE)
      # ~~~~ END CODE INSERT ~~~~

      # Parse response to check status code and redirection
      try:
        response_str = response_bytes.decode('utf-8', 'ignore')
        response_lines = response_str.split('\r\n')
        status_line = response_lines[0]
        status_code = int(status_line.split(' ')[1])
        
        # Check if it's a redirect (301 or 302)
        location = None
        if status_code in [301, 302]:
          for line in response_lines:
            if line.lower().startswith('location:'):
              location = line.split(':', 1)[1].strip()
              break
          if location:
            print(f"Detected {status_code} redirect to {location}")
      except:
        print("Error parsing response headers")
        status_code = 200  # Default value
      
      # Parse Cache-Control header
      max_age = None
      try:
        for line in response_lines:
          if line.lower().startswith('cache-control:'):
            cache_control = line.split(':', 1)[1].strip()
            if 'max-age=' in cache_control:
              max_age_part = re.search(r'max-age=(\d+)', cache_control)
              if max_age_part:
                max_age = int(max_age_part.group(1))
                print(f"Found max-age: {max_age} seconds")
                break
      except:
        print("Error parsing Cache-Control header")

      # Send the response to the client
      # ~~~~ INSERT CODE ~~~~
      clientSocket.sendall(response_bytes)
      # ~~~~ END CODE INSERT ~~~~

      # Create a new file in the cache for the requested file.
      cacheDir, file = os.path.split(cacheLocation)
      print ('cached directory ' + cacheDir)
      if not os.path.exists(cacheDir):
        os.makedirs(cacheDir)
      cacheFile = open(cacheLocation, 'wb')

      # Save origin server response in the cache file
      # ~~~~ INSERT CODE ~~~~
      # Determine if the response should be cached
      should_cache = True

      # For 302 temporary redirects, typically don't cache
      if status_code == 302:
        should_cache = False
        print("302 temporary redirect - not caching")

      # If we should cache it, save the response and metadata
      if should_cache:
        cacheFile.write(response_bytes)
        # If max-age is specified, save metadata
        if max_age is not None:
          cache_time = int(time.time())  # Current timestamp
          expiry_time = cache_time + max_age
          
          try:
            meta_file = open(cacheLocation + ".meta", 'w')
            meta_file.write(f"cached_at: {cache_time}\nexpires_at: {expiry_time}\nmax_age: {max_age}\n")
            meta_file.close()
            print(f"Saved cache metadata with expiry in {max_age} seconds")
          except:
            print("Error saving cache metadata")
      # ~~~~ END CODE INSERT ~~~~
      cacheFile.close()
      print ('cache file closed')

      # finished communicating with origin server - shutdown socket writes
      print ('origin response received. Closing sockets')
      originServerSocket.close()
       
      clientSocket.shutdown(socket.SHUT_WR)
      print ('client socket shutdown for writing')
    except OSError as err:
      print ('origin server request failed. ' + err.strerror)

  try:
    clientSocket.close()
  except:
    print ('Failed to close client socket')
