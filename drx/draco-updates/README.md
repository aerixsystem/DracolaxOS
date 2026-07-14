<div align="left">
  <table>
    <tr>
      <td valign="middle">
        <img src="./icon.png" width="100" height="100" alt="draco-updates icon">
      </td>
      <td valign="middle">
        <h1>draco-updates</h1>
        <p>This repository stores DracolaxOS update metadata and release files.</p>
      </td>
    </tr>
  </table>
</div>

<hr>

<h2>Purpose</h2>
<p>DracolaxOS uses this repository as an update source for:</p>
<ul>
  <li>online updates</li>
  <li>offline update packages</li>
  <li>release bundles</li>
  <li>stable and test versions</li>
</ul>

<h2>Repository layout</h2>
<pre><code>index.json
latest.json
stable/
beta/
releases/</code></pre>

<h3><code>index.json</code></h3>
<p>The main update index.</p>
<p>It lists components, versions, download links, and update flags.</p>

<h3><code>latest.json</code></h3>
<p>A small stable pointer file.</p>
<p>It tells the system which versions are current.</p>

<h3><code>stable/</code></h3>
<p>Contains tested and safe update packages.</p>

<h3><code>beta/</code></h3>
<p>Contains experimental or test packages.</p>

<h3><code>releases/</code></h3>
<p>Contains release bundles and packaged system update files.</p>

<hr>

<h2>Update flow</h2>
<p>DracolaxOS reads <code>index.json</code>, compares it with the local system manifest, downloads the needed package, stages it, validates it, then applies it.</p>
<p>If the update fails, the system can rollback to the previous version.</p>

<hr>

<h2>File format</h2>
<p>Update packages use the <code>.drxpkg</code> format.</p>
<p>A package should contain:</p>
<ul>
  <li>a package manifest</li>
  <li>update files</li>
  <li>checksums</li>
  <li>optional scripts</li>
</ul>

<hr>

<h2>Local offline updates</h2>
<p>Users can also download a <code>.drxpkg</code> file and install it locally.</p>
<p>This is useful when the system has no internet access or when a known good version is needed.</p>

<hr>

<h2>Release policy</h2>
<p>Only push releases that are tested.</p>
<p>Use stable for normal users.</p>
<p>Use beta only for testing.</p>

<hr>

<h2>License</h2>
<p>MIT</p>
