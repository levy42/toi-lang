<!doctype html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>{{ title }}</title>
    <link rel="stylesheet" href="{{ css_href }}">
    <link rel="icon" type="image/svg+xml" href="{{ favicon_href }}">
</head>
<body>
    <aside>
    <a class="brand" href="{{ home_href }}">
    <img class="brand-logo" src="{{ logo_href }}" alt="Toi logo">
    <span>Toi Docs</span>
    </a>
    <ul class="nav">
    {% for item in nav_entries %}
    {% if item.kind == "page" %}
    <li{% if item.active %} class="active"{% endif %}><a href="{{ item.href }}">{{ item.title }}</a></li>
    {% else %}
    <li class="nav-collapsible">
    <details class="nav-group{% if item.open %} active{% endif %}"{% if item.open %} open{% endif %}>
    <summary>{{ item.label }}</summary>
    <ul class="subnav">
    {% for child in item.children %}
    <li{% if child.active %} class="active"{% endif %}><a href="{{ child.href }}">{{ child.title }}</a></li>
    {% endfor %}
    </ul>
    </details>
    </li>
    {% endif %}
    {% endfor %}
    </ul>
    </aside>
    <main>{{ body_html }}</main>
</body>
</html>
