# Admin framework

`lib.admin` adds server-rendered CRUD views to a `lib.http_server` app and
`lib.db` handle. Existing `admin.register(Record, options)` registrations remain
supported. Structured views can use `ModelView`, `Field`, and `Column`:

```toi
admin_mod = import lib.admin
html = import lib.html

fn authorize_admin(req, action, view)
  return current_user(req).is_admin

fn csrf_token_for(req, mode, view)
  return session_csrf_token(req)

fn check_csrf(req, action, view)
  return verify_csrf(req)

fn validate_user(row, operation, req)
  if row.name == ""
    return "Name is required"
  return nil

fn normalize_user(row, operation, req)
  row.email = row.email.lower()
  return row

fn render_user_id(value, row, column)
  return html.tag("code", nil, html.text(value))

admin = admin_mod.new(app, db, {
  title = "Operations",
  mount_path = "/admin",
  authorize = authorize_admin,
  csrf_token = csrf_token_for,
  csrf_check = check_csrf
})

UserView = admin_mod.ModelView(User, {
  label = "Users",
  fields = {
    admin_mod.Field("name", {label = "Full name", required = true}),
    admin_mod.Field("email", {kind = "email", required = true}),
    admin_mod.Field("role", {
      kind = "select",
      choices = {admin = "Administrator", member = "Member"}
    }),
    admin_mod.Field("notes", {kind = "textarea", rows = 8, help = "Internal only"}),
    admin_mod.Field("id", {readonly = true})
  },
  columns = {
    admin_mod.Column("name", {label = "Name"}),
    admin_mod.Column("email", {label = "Email address"}),
    admin_mod.Column("role", {sortable = false}),
    admin_mod.Column("id", {label = "ID", render = render_user_id})
  },
  search_fields = {"name", "email"},
  validate = validate_user,
  before_save = normalize_user
})
admin.register(UserView)
```

## ModelView options

- `label`: navigation and page label.
- `fields` (or legacy `form_fields`): strings or `Field` specs. Defaults to
  schema fields except `id`.
- `columns` (or legacy `list_fields`): strings or `Column` specs. Defaults to
  `id` followed by form fields.
- `detail_fields`: strings or `Field` specs shown on the read-only detail page.
- `search_fields`, `default_sort`, `per_page`: list behavior.
- `field_types`: legacy field-name-to-kind map.
- `validate(row, operation, req)`: return an error string to redisplay the form,
  or nil to accept the row.
- `before_save(row, operation, req)`: return the row to persist.
- `after_delete(id, req)`: called after deletion.

Field specs support `label`, `kind` or `widget`, `required`, `readonly`,
`editable`, `disabled`, `placeholder`, `help`, `rows`, `choices`, `attrs`, and
`default`. `kind` supports `text`, `email`, `url`, `date`, `int`, `number`,
`bool`, `textarea`, `select`, and `hidden`. A `format(value, row)` callback
returns display text and is escaped. A `render(value, row, spec)` callback
returns trusted HTML.

Column specs support `name`, `label`, `sortable`, `format(value, row)`, and
`render(value, row, spec)`. Rendering callback output is trusted HTML; escape
untrusted values with `lib.html.text` before composing markup.

## Access and CSRF

`authorize(req, action, view)` runs for every admin route. Return true to allow
the request; any false or nil result returns HTTP 403. Actions are `index`,
`list`, `detail`, `new`, `create.post`, `edit`, `edit.post`, and `delete`.

For mutations, `csrf_token(req, mode, view)` can add a hidden `_csrf` field to
the form. `csrf_check(req, action, view)` must verify the submitted token and
return true. The library does not manage sessions or provide a built-in token
store; configure both callbacks when using cookie-based authentication.

## htmx

The layout loads htmx 2 from the configured `htmx_src` URL (defaults to the
unpkg distribution). Set `htmx_src` to a locally served asset to avoid the
external dependency. List search, sorting, pagination, new/edit navigation,
forms, and row deletion use htmx when available. Ordinary links and forms
remain valid without JavaScript.
