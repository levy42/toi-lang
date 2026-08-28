<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>{{ page_title }}</title>
    <style>{{ styles }}</style>
  </head>
  <body>
    <aside id="sidebar">
      <h1>{{ app_title }}</h1>
      <nav>
        {% for item in nav_items %}
        <a href="{{ item.href }}">{{ item.label }}</a>
        {% endfor %}
      </nav>
    </aside>
    <main>
      {{ body_html }}
    </main>
  </body>
</html>
